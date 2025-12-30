// Copyright (c) 2018-2025, The Monero Project
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

#include <boost/optional/optional.hpp>
#include <functional>

#include "cryptonote_basic/cryptonote_basic.h"
#include "db/account.h"
#include "db/storage.h"
#include "span.h"

namespace lws
{
  class subaddress_reader
  {
  public:
    subaddress_reader(db::storage const& disk_in, const std::uint32_t max_subaddresses);

    expect<db::address_index> find_subaddress(const account& user, crypto::public_key const& pubkey);
    expect<void> update_lookahead(const account& user, const db::address_index& match, const db::block_id height);

  private:
    expect<db::storage_reader> reader;
    std::optional<db::storage> disk;
    db::cursor::subaddress_indexes cur;
    const std::uint32_t max_subaddresses;

    void update_reader();
  };

  class ownership_test {
  public:
    using spend_action = std::function<void(account&, db::spend&&)>;
    using output_action = std::function<void(account&, db::output&&)>;

    ownership_test(spend_action, output_action);

    ownership_test(const ownership_test&) = delete;
    ownership_test(ownership_test&&) = default;

    ownership_test& operator=(const ownership_test&) = delete;
    ownership_test& operator=(ownership_test&&) = default;

    /*! Tests the transaction against out accounts,
      and invokes callbacks for matching inputs or outputs.
      @param height mined height
      @param timestamp mined block timestamp
      @param out_ids maps vout indices to global utxo indexes
    */
    void operator()(
      epee::span<account> users,
      db::block_id height,
      std::uint64_t timestamp,
      const crypto::hash& tx_hash,
      const cryptonote::transaction& tx,
      const std::vector<std::uint64_t>& out_ids
    );

    void enable_subaddresses(const db::storage& disk, const std::uint32_t max_subaddresses);
    void disable_subaddresses();

  private:
    spend_action on_spend;
    output_action on_output;

    boost::optional<subaddress_reader> subaddress;
  };

} // namespace lws
