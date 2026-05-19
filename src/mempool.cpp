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

#include <algorithm>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/thread/lock_guard.hpp>
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

  void mempool::add_txs(epee::span<cryptonote::transaction> txs)
  {
    const auto now = std::chrono::system_clock::now();
    const boost::lock_guard<boost::mutex> lock{mutex_};
    snapshot_.reset();

    for (auto& tx: txs)
    {
      auto found = state_.try_emplace(get_transaction_hash(tx), nullptr);
      if (!found.first->second)
        found.first->second = std::make_shared<pool_entry>(std::move(tx), now);
    }
  }

  void mempool::reset_txs(pool_table&& txs)
  {
    const boost::lock_guard<boost::mutex> lock{mutex_};
    snapshot_.reset();

    MDEBUG("Mempool reset from size " << state_.size() << " to " << txs.size());
    state_ = std::move(txs);
  }

  std::unordered_set<crypto::hash> mempool::address_cache::get(const db::account_address& address) const
  {
    auto it = map_.find(address);
    if (it == map_.end())
      return {};
    return {it->second.txids.begin(), it->second.txids.end()};
  }

  void mempool::address_cache::set(const db::account_address& address, std::vector<crypto::hash>&& txids)
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
    std::unordered_set<crypto::hash> skip;
    std::shared_ptr<const pool_snapshot> snapshot;
    {
      const boost::lock_guard<boost::mutex> lock{mutex_};
      skip = cache_.get(user.db_address());

      if (!snapshot_)
      {
         // otherwise make a fresh snapshot
        pool_snapshot temp;
        temp.resize(state_.size());
        std::copy(state_.begin(), state_.end(), temp.begin());
        snapshot_ = std::make_shared<const pool_snapshot>(std::move(temp));
      }
      snapshot = snapshot_;
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
    for (const auto& pair: *snapshot) {
      if (!skip.count(pair.first))
        scan_transaction(
          epee::span<lws::account>(&user, 1),
          db::block_id::txpool,
          boost::numeric_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(pair.second->timestamp)),
          std::addressof(pair.first),
          pair.second->tx,
          fake_outs,
          false
        );
      if (found.empty() || found.back().hash != pair.first)
        skipped.push_back(pair.first);
    }

    {
      const boost::lock_guard<boost::mutex> lock{mutex_};
      cache_.set(user.db_address(), std::move(skipped));
    }

    return found;
  }

  expect<void> pool_update_loop(std::shared_ptr<mempool> pool, rpc::client& client)
  {
    LWS_VERIFY(pool);
    return client.event_loop(
      [pool](std::string&& json) -> expect<void>
      {
        auto msg = rpc::parse_json_response<rpc::get_transaction_pool>(std::move(json));
        if (!msg)
        {
          MERROR("Pool failed to parse block response" << msg.error());
          return msg.error();
        }

        mempool::pool_table txs{};
        txs.reserve(msg->transactions.size());
        const auto now = std::chrono::system_clock::now();
        for (auto& tx: msg->transactions)
        {
          auto shared_tx = std::make_shared<mempool::pool_entry>(std::move(tx.tx), now);
          txs.emplace(tx.tx_hash, std::move(shared_tx));
        }
        pool->reset_txs(std::move(txs));
        return {};
      },

      [&client](rpc::minimal_chain_pub&&) -> expect<void>
      {
        cryptonote::rpc::GetTransactionPool::Request req{};

        expect<void> status = success();
        for (unsigned i = 0; i < 2; ++i)
        {
          status = client.send(
            rpc::client::make_message("get_transaction_pool", req), std::chrono::seconds(0)
          );
          if (status || status != net::zmq::make_error_code(EFSM))
            return status;
          MONERO_CHECK(client.daemon_reconnect());
        }
        return status;
      },

      [pool](rpc::full_txpool_pub&& msg) -> expect<void>
      {
        pool->add_txs(epee::to_mut_span(msg.txes));
        return {};
      }
    );
  }

} // namespace lws
