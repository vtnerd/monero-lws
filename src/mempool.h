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

#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "common/expect.h"
#include "cryptonote_basic/cryptonote_basic.h" // external/monero/src
#include "db/fwd.h"
#include "rpc/client.h"

namespace lws
{
  struct found_pool_tx {
    crypto::hash hash;
    std::vector<db::spend> spends;
    std::vector<db::output> outputs;
  };

  /*!
    Thread-safe mempool cache, designed to perform well with a single writer
    and multiple concurrent readers.

    The primary mutable state is guarded with a mutex, while each reader
    receives an immutable snapshot of the latest state. These snapshots
    use structural sharing to avoid copying the heavyweight transactions.

    The two update methods are `add_tx` and `filter_txs`.
  */
  class mempool
  {
  public:
    using pool_table = std::unordered_map<
      crypto::hash,
      std::shared_ptr<const cryptonote::transaction>
    >;

    //! \return Snapshot of current mempool state. Thread-safe.
    std::shared_ptr<const pool_table> get_txs() const;

    //! Adds transactions to the pool, if they are not already present.
    // Thread-safe.
    void add_txs(epee::span<cryptonote::transaction>);

    //! Replaces all transactions to the pool. Thread-safe.
    void reset_txs(pool_table&&);

    //! Scans the pool for transactions belonging to an address. Thread-safe.
    std::vector<found_pool_tx> scan_account(lws::account& user) const;

  private:
    //! Remembers which txid's that do *not* belong to an address,
    // since there is no need for the scanner to revisit these.
    class address_cache
    {
    public:
      std::unordered_set<crypto::hash> get(const std::string& address) const;
      void set(const std::string& address, std::vector<crypto::hash>&& txids);

    private:
      struct entry
      {
        std::vector<crypto::hash> txids;
        std::list<std::string>::iterator order;
      };

      std::unordered_map<std::string, entry> map_;
      std::list<std::string> order_;
    };

    pool_table state_;
    mutable address_cache cache_;
    mutable std::shared_ptr<const pool_table> snapshot_;
    mutable std::mutex mutex_;
  };

  //! Run this on a thread to keep the pool in sync.
  // Loops until an error or shutdown signal occurs.
  expect<void> pool_update_loop(std::shared_ptr<mempool> pool, rpc::client& client);

} // namespace lws
