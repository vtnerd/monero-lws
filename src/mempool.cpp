// Copyright (c) 2024, The Monero Project
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "mempool.h"

#include "common/error.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "error.h"
#include "misc_log_ex.h"
#include "rpc/daemon_messages.h" // external/monero/src
#include "rpc/daemon_zmq.h"
#include "rpc/json.h"
#include "util/ownership_test.h"

namespace lws
{
  const size_t max_cache_size = 1000;

  std::shared_ptr<const mempool::pool_table> mempool::get_txs() const
  {
    const std::lock_guard<std::mutex> lock{mutex_};

    // return the existing snapshot if we have one
    if (snapshot_) return snapshot_;

    // otherwise make a fresh snapshot
    snapshot_ = std::make_shared<const pool_table>(state_);
    return snapshot_;
  }

  void mempool::add_txs(epee::span<cryptonote::transaction> txs)
  {
    const std::lock_guard<std::mutex> lock{mutex_};
    snapshot_.reset();

    for (auto& tx: txs)
    {
      auto hash = get_transaction_hash(tx);
      auto found = state_.find(hash);
      if (found != state_.end()) continue;
      state_.insert({
        hash,
        std::make_shared<const cryptonote::transaction>(std::move(tx))
      });
      // MDEBUG("Mempool added tx " << hash << " size: " << state_.size());
    }
  }

  void mempool::reset_txs(pool_table&& txs)
  {
    const std::lock_guard<std::mutex> lock{mutex_};
    snapshot_.reset();

    MDEBUG("Mempool reset from size " << state_.size() << " to " << txs.size());
    state_ = std::move(txs);
  }

  std::unordered_set<crypto::hash> mempool::address_cache::get(const std::string& address) const
  {
    auto it = map_.find(address);
    if (it == map_.end())
      return {};
    return {it->second.txids.begin(), it->second.txids.end()};
  }

  void mempool::address_cache::set(const std::string& address, std::vector<crypto::hash>&& txids)
  {
    // find or create entry for this address
    auto status = map_.emplace(address, entry{});
    entry& e = status.first->second;
    e.txids = std::move(txids);
    if (status.second)
    {
      // add new address to order list
      order_.push_front(address);
      e.order = order_.begin();

      // evict old address if needed
      if (map_.size() >= max_cache_size)
      {
        auto& evict_address = order_.back();
        map_.erase(evict_address);
        order_.pop_back();
      }
    }
    else
      // move existing address to front of order list
      order_.splice(order_.begin(), order_, e.order);
  }

  std::vector<found_pool_tx> mempool::scan_account(lws::account& user) const
  {
    std::shared_ptr<const pool_table> snapshot = get_txs();
    std::unordered_set<crypto::hash> skip;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      skip = cache_.get(user.address());
    }

    std::vector<found_pool_tx> found{};
    ownership_test scan_transaction{
      [&found](account&, db::spend&& spend)
      {
        if (found.empty() || found.back().hash != spend.link.tx_hash)
          found.push_back({spend.link.tx_hash});
        found.back().spends.push_back(std::move(spend));
      },
      [&found](account&, db::output&& output)
      {
        if (found.empty() || found.back().hash != output.link.tx_hash)
          found.push_back({output.link.tx_hash});
        found.back().outputs.push_back(std::move(output));
      }
    };

    // uint64::max is for txpool
    static const std::vector<std::uint64_t> fake_outs(
      256, std::numeric_limits<std::uint64_t>::max()
    );

    std::vector<crypto::hash> skipped;
    for (auto &pair: *snapshot) {
      if (!skip.count(pair.first))
        scan_transaction(
          epee::span<lws::account>(&user, 1),
          db::block_id::txpool,
          0,
          pair.first,
          *pair.second,
          fake_outs
        );
      if (found.empty() || found.back().hash != pair.first)
        skipped.push_back(pair.first);
    }

    {
      std::lock_guard<std::mutex> lock{mutex_};
      cache_.set(user.address(), std::move(skipped));
    }

    return found;
  }

  expect<void> pool_update_loop(std::shared_ptr<mempool> pool, rpc::client& client)
  {
    return client.event_loop(
      [&pool](std::string&& json) -> expect<void>
      {
        const auto msg = rpc::parse_json_response<rpc::get_transaction_pool>(std::move(json));
        if (!msg)
        {
          MERROR("Pool failed to parse block response" << msg.error());
          return msg.error();
        }

        mempool::pool_table txs{};
        txs.reserve(msg->transactions.size());
        for (auto& tx: msg->transactions)
        {
          auto shared_tx = std::make_shared<const cryptonote::transaction>(std::move(tx.tx));
          txs.insert({tx.tx_hash, std::move(shared_tx)});
        }
        pool->reset_txs(std::move(txs));
        return {};
      },

      [&client](rpc::minimal_chain_pub&& msg) -> expect<void>
      {
        cryptonote::rpc::GetTransactionPool::Request req{};
        const auto sent = client.send(
          rpc::client::make_message("get_transaction_pool", req),
          std::chrono::seconds(0));

        // fine if the socket is busy, we just carry extra txs for a while
        if (!sent && sent != lws::error::daemon_timeout && sent != net::zmq::make_error_code(EFSM))
        {
          MERROR("Pool failed to send block request" << sent.error());
          return sent.error();
        }
        return {};
      },

      [&pool](rpc::full_txpool_pub&& msg) -> expect<void>
      {
        pool->add_txs(epee::to_mut_span(msg.txes));
        return {};
      }
    );
  }

} // namespace lws
