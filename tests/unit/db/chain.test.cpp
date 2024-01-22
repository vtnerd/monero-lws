// Copyright (c) 2023, The Monero Project
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

#include "chain.test.h"

#include <cstdint>
#include "db/storage.test.h"
#include "error.h"

namespace lws_test
{
  void test_chain(lest::env& lest_env, lws::db::storage_reader reader, lws::db::block_id id, epee::span<const crypto::hash> snapshot)
  {
    EXPECT(1 <= snapshot.size());

    std::uint64_t d = std::uint64_t(id);
    for (const auto& hash : snapshot)
    {
      SETUP("Testing Block #: " + std::to_string(d))
      {
        EXPECT(MONERO_UNWRAP(reader.get_block_hash(lws::db::block_id(d))) == hash);
        ++d;
      }
    }

    const lws::db::block_info last_block =
      MONERO_UNWRAP(reader.get_last_block());
    EXPECT(last_block.id == lws::db::block_id(d - 1));
    EXPECT(last_block.hash == snapshot[snapshot.size() - 1]);
  }
}

LWS_CASE("db::storage::sync_chain")
{
  lws::db::account_address account{};
  crypto::secret_key view{};
  crypto::generate_keys(account.spend_public, view);
  crypto::generate_keys(account.view_public, view);
 
  SETUP("Appended Chain")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());

    const auto get_account = [&db, &account] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account)).second;
    };
   
    const crypto::hash chain[5] = {
      last_block.hash,
      crypto::rand<crypto::hash>(),
      crypto::rand<crypto::hash>(),
      crypto::rand<crypto::hash>(),
      crypto::rand<crypto::hash>()
    };

    EXPECT(db.add_account(account, view));
    EXPECT(db.sync_chain(lws::db::block_id(0), chain) == lws::error::bad_blockchain);
    EXPECT(db.sync_chain(last_block.id, {chain + 1, 4}) == lws::error::bad_blockchain);
    EXPECT(db.sync_chain(last_block.id, chain));

    {
      const lws::account accounts[1] = {lws::account{get_account(), {}, {}}};
      EXPECT(accounts[0].scan_height() == last_block.id);
      EXPECT(db.update(last_block.id, chain, accounts));
      EXPECT(get_account().scan_height == lws::db::block_id(std::uint64_t(last_block.id) + 4));
    }

    SECTION("Verify Append")
    {
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, chain);
    }

    SECTION("Fork Chain")
    {
      const crypto::hash fchain[5] = {
        chain[0],
        chain[1],
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>()
      };

      EXPECT(db.sync_chain(last_block.id, fchain));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, fchain);
      EXPECT(get_account().scan_height == lws::db::block_id(std::uint64_t(last_block.id) + 1));
    }
  }
}

