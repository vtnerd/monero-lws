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

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "db/fwd.h"
#include "cryptonote_basic/cryptonote_basic.h" // external/monero/src
#include "rpc/client.h"

namespace lws
{
  /*!
    Thread-safe mempool cache using copy-on-write snapshots.

    Writers clone the current snapshot, apply their mutation, and then replace
    the shared pointer. Readers clone the shared pointer under a short lock and
    can iterate the snapshot without additional synchronisation.
  */
  class mempool
  {
  public:
    using txs = std::vector<std::shared_ptr<const cryptonote::transaction>>;

    mempool();
    ~mempool();

    //! \return Snapshot of current mempool state. Thread-safe.
    std::shared_ptr<txs> get() const;

    expect<void> start(rpc::context& context);
    void stop();

  private:
    struct thread_detail;

    static crypto::hash get_hash(cryptonote::transaction const& tx);

    mutable std::mutex mutex_;
    std::shared_ptr<txs> current_;

    std::unique_ptr<std::thread> thread_;
    std::unique_ptr<thread_detail> updater_;
  };

  struct found_pool_tx {
    crypto::hash hash;
    std::vector<db::spend> spends;
    std::vector<db::output> outputs;
  };

  std::vector<found_pool_tx> scan_txpool(mempool& pool, lws::account& user);

} // namespace lws
