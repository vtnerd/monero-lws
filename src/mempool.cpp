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
#include <system_error>
#include <unordered_set>

#include "common/error.h"
#include "common/expect.h"
#include "crypto/hash.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "error.h"
#include "misc_log_ex.h"
#include "rpc/daemon_messages.h" // external/monero/src
#include "rpc/daemon_zmq.h"
#include "rpc/json.h"
#include "util/ownership_test.h"

namespace lws
{
  std::chrono::seconds send_timeout {3};
  std::chrono::seconds receive_timeout {5};

  struct mempool::thread_detail
  {
    mempool& self;
    rpc::context &context;
    rpc::client client;
    std::atomic<bool> stopped;

    thread_detail(mempool& self, rpc::context& ctx, rpc::client&& rpc_client)
      : self(self)
      , context(ctx)
      , client(std::move(rpc_client))
      , stopped(false)
    {}

    void run()
    {
      while (!stopped.load(std::memory_order_acquire) && client)
      {
        auto pub = client.wait_for_pub();
        if (!pub)
        {
          if (pub.matches(std::errc::interrupted) || pub.matches(std::errc::invalid_argument))
            break;
          if (pub == lws::error::daemon_timeout || pub == net::zmq::make_error_code(EAGAIN))
          {
            auto synced = sync_pool();
            if (!synced)
              MERROR("Unable to sync mempool: " << synced.error());
            continue;
          }
          MERROR("Mempool loop failed: " << pub.error().message());
          continue;
        }

        if (stopped.load(std::memory_order_acquire) || !client)
          break;

        switch(pub->first)
        {
          case rpc::client::topic::txpool:
            handle_txpool(std::move(pub->second));
            break;

          case rpc::client::topic::block:
            handle_block(std::move(pub->second));
            break;
        }
      }
    }

    void handle_block(std::string&& json) {
      const auto parsed = rpc::minimal_chain_pub::from_json(std::move(json));
      if (!parsed)
      {
        MERROR("Unable to parse blockchain notification: " << parsed.error());
        return;
      }
      MDEBUG("Mempool block " <<  parsed->top_block_height);

      auto synced = sync_pool();
      if (!synced)
        MERROR("Unable to sync mempool: " << synced.error());
    }

    void handle_txpool(std::string&& json) {
      const auto parsed = rpc::full_txpool_pub::from_json(std::move(json));
      if (!parsed)
      {
        MERROR("Failed parsing txpool pub: " << parsed.error().message());
        return;
      }

      const std::lock_guard<std::mutex> lock{self.mutex_};
      auto next = std::make_shared<mempool::txs>(*self.current_);
      next->reserve(parsed->txes.size());
      for (auto& tx: parsed->txes)
      {
        MDEBUG("Mempool tx " << get_transaction_hash(tx) << " size: " << next->size());
        auto shared_tx = std::make_shared<const cryptonote::transaction>(std::move(tx));
        next->push_back(std::move(shared_tx));
      }
      self.current_ = next;
    }


    expect<void> sync_pool () {
      auto upstream = fetch_pool();
      if (!upstream)
        return upstream.error();

      const std::lock_guard<std::mutex> lock{self.mutex_};
      auto next = std::make_shared<mempool::txs>();
      next->reserve(upstream->transactions.size());
      for (auto& tx: upstream->transactions)
      {
        auto shared_tx = std::make_shared<const cryptonote::transaction>(std::move(tx.tx));
        next->push_back(std::move(shared_tx));
      }
      self.current_ = next;

      return success();
    }

    expect<rpc::get_transaction_pool_response> fetch_pool() {
      cryptonote::rpc::GetTransactionPool::Request req{};
      const auto sent = client.send(rpc::client::make_message("get_transaction_pool", req), send_timeout);
      if (!sent)
        return sent.error();
      auto resp = client.get_message(receive_timeout);
      if (!resp)
        return resp.error();
      return rpc::parse_json_response<rpc::get_transaction_pool>(std::move(*resp));
    }
  };

  mempool::mempool()
    : mutex_()
    , current_(std::make_shared<txs>())
    , thread_()
    , updater_()
  {}

  mempool::~mempool()
  {
    stop();
  }

  std::shared_ptr<mempool::txs> mempool::get() const
  {
    const std::lock_guard<std::mutex> lock{mutex_};
    return current_;
  }

  expect<void> mempool::start(rpc::context& context)
  {
    stop();

    auto client = context.connect();
    if (!client)
      return client.error();

    updater_ = std::make_unique<thread_detail>(*this, context, std::move(client).value());
    thread_ = std::make_unique<std::thread>(
      [updater = updater_.get()]()
      {
        try
        {
          updater->run();
        }
        catch (const std::exception& e)
        {
          MERROR("Mempool thread terminated: " << e.what());
        }
        catch (...)
        {
          MERROR("Mempool thread terminated with unknown error");
        }
      }
    );

    return success();
  }

  void mempool::stop()
  {
    if (updater_)
    {
      updater_->stopped.store(true, std::memory_order_release);
      const auto rc = updater_->context.raise_abort_process();
      if (!rc)
        MERROR("Failed to signal mempool thread: " << rc.error().message());
      updater_.reset();
    }
    if (thread_)
    {
      if (thread_->joinable())
        thread_->join();
      thread_.reset();
    }
  }

  std::vector<found_pool_tx> scan_txpool(mempool& pool, lws::account &user)
  {
    // TODO: The pool should be indexed by hash:
    std::shared_ptr<lws::mempool::txs> snap = pool.get();
    std::unordered_map<crypto::hash, cryptonote::transaction> snapshot{};
    for (auto &tx: *snap) {
      snapshot.insert({ get_transaction_hash(*tx), *tx });
    }

    std::vector<found_pool_tx> out{};

    ownership_test scan_transaction{
      [&out](account&, db::spend&& spend)
      {
        if (out.empty() || out.back().hash != spend.link.tx_hash)
          out.push_back({ spend.link.tx_hash });
        out.back().spends.push_back(std::move(spend));
      },
      [&out](account&, db::output&& output)
      {
        if (out.empty() || out.back().hash != output.link.tx_hash)
          out.push_back({ output.link.tx_hash });
        out.back().outputs.push_back(std::move(output));
      }
    };

    // uint64::max is for txpool
    static const std::vector<std::uint64_t> fake_outs(
      256, std::numeric_limits<std::uint64_t>::max()
    );

    for (auto &pair: snapshot) {
      crypto::hash hash;
      scan_transaction(
        epee::span<lws::account>(&user, 1),
        pair.first,
        pair.second,
        db::block_id::txpool,
        0,
        fake_outs
      );
    }

    return out;
  }
} // namespace lws
