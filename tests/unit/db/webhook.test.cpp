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

#include "framework.test.h"

#include <boost/uuid/random_generator.hpp>
#include <cstdint>
#include "crypto/crypto.h" // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "db/storage.test.h"

namespace
{
  bool add_out(lws::account& account, const lws::db::block_id last_id, const std::uint64_t payment_id)
  {
    crypto::hash8 real_id{};
    std::memcpy(std::addressof(real_id), std::addressof(payment_id), sizeof(real_id));
    return account.add_out(
      lws::db::output{
        lws::db::transaction_link{
          lws::db::block_id(lmdb::to_native(last_id) + 1),
          crypto::rand<crypto::hash>()
        },
        lws::db::output::spend_meta_{
          lws::db::output_id{0, 100},
          std::uint64_t(1000),
          std::uint32_t(16),
          std::uint32_t(1),
          crypto::rand<crypto::public_key>()
        },
        std::uint64_t(10000000),
        std::uint64_t(0),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::public_key>(),
        crypto::rand<rct::key>(),
        {{}, {}, {}, {}, {}, {}, {}},
        lws::db::extra_and_length(0),
        lws::db::output::payment_id_{real_id}
      }
    );
  }
}

LWS_CASE("db::storage::*_webhook")
{
  lws::db::account_address account{};
  crypto::secret_key view{};
  crypto::generate_keys(account.spend_public, view);
  crypto::generate_keys(account.view_public, view);

  SETUP("One Account and one Webhook Database")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());
    MONERO_UNWRAP(db.add_account(account, view));

    const boost::uuids::uuid id = boost::uuids::random_generator{}();
    {
      lws::db::webhook_value value{
        lws::db::webhook_dupsort{500, id},
        lws::db::webhook_data{"http://the_url", "the_token", 3}
      };
      MONERO_UNWRAP(
        db.add_webhook(lws::db::webhook_type::tx_confirmation, account, std::move(value))
      );
    }

    SECTION("storage::get_webhooks()")
    {
      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      const auto result = MONERO_UNWRAP(reader.get_webhooks());
      EXPECT(result.size() == 1);
      EXPECT(result[0].first.user == lws::db::account_id(1));
      EXPECT(result[0].first.type == lws::db::webhook_type::tx_confirmation);
      EXPECT(result[0].second.size() == 1);
      EXPECT(result[0].second[0].first.payment_id == 500);
      EXPECT(result[0].second[0].first.event_id == id);
      EXPECT(result[0].second[0].second.url == "http://the_url");
      EXPECT(result[0].second[0].second.token == "the_token");
      EXPECT(result[0].second[0].second.confirmations == 3);
    }

    SECTION("storage::clear_webhooks(addresses)")
    {
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).size() == 1);
      MONERO_UNWRAP(db.clear_webhooks({std::addressof(account), 1}));

      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      const auto result = MONERO_UNWRAP(reader.get_webhooks());
      EXPECT(result.empty());
    }

    SECTION("storage::clear_webhooks(uuid)")
    {
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).size() == 1);
      MONERO_UNWRAP(db.clear_webhooks({id}));

      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      const auto result = MONERO_UNWRAP(reader.get_webhooks());
      EXPECT(result.empty());
    }

    SECTION("storage::update(...) one at a time")
    {
      lws::account full_account = lws::db::test::make_account(account, view);
      full_account.updated(last_block.id);
      EXPECT(add_out(full_account, last_block.id, 500));

      const std::vector<lws::db::output> outs = full_account.outputs();
      EXPECT(outs.size() == 1);

      lws::db::block_info head = last_block;
      for (unsigned i = 0; i < 1; ++i)
      {
        crypto::hash chain[2] = {head.hash, crypto::rand<crypto::hash>()};

        auto updated = db.update(head.id, chain, {std::addressof(full_account), 1});
        EXPECT(!updated.has_error());
        EXPECT(updated->first == 1);
        if (i < 3)
        {
          EXPECT(updated->second.size() == 1);
          EXPECT(updated->second[0].key.user == lws::db::account_id(1));
          EXPECT(updated->second[0].key.type == lws::db::webhook_type::tx_confirmation);
          EXPECT(updated->second[0].value.first.payment_id == 500);
          EXPECT(updated->second[0].value.first.event_id == id);
          EXPECT(updated->second[0].value.second.url == "http://the_url");
          EXPECT(updated->second[0].value.second.token == "the_token");
          EXPECT(updated->second[0].value.second.confirmations == i + 1);

          EXPECT(updated->second[0].tx_info.link == outs[0].link);
          EXPECT(updated->second[0].tx_info.spend_meta.id == outs[0].spend_meta.id);
          EXPECT(updated->second[0].tx_info.pub == outs[0].pub);
          EXPECT(updated->second[0].tx_info.payment_id.short_ == outs[0].payment_id.short_);
        }
        else
          EXPECT(updated->second.empty());

        full_account.updated(head.id);
        head = {lws::db::block_id(lmdb::to_native(head.id) + 1), chain[1]};
      }
    }

    SECTION("storage::update(...) all at once")
    {
      const crypto::hash chain[5] = {
        last_block.hash,
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>()
      };

      lws::account full_account = lws::db::test::make_account(account, view);
      full_account.updated(last_block.id);
      EXPECT(add_out(full_account, last_block.id, 500));

      const std::vector<lws::db::output> outs = full_account.outputs();
      EXPECT(outs.size() == 1);

      const auto updated = db.update(last_block.id, chain, {std::addressof(full_account), 1});
      EXPECT(!updated.has_error());
      EXPECT(updated->first == 1);
      EXPECT(updated->second.size() == 3);

      for (unsigned i = 0; i < 3; ++i)
      {
        EXPECT(updated->second[i].key.user == lws::db::account_id(1));
        EXPECT(updated->second[i].key.type == lws::db::webhook_type::tx_confirmation);
        EXPECT(updated->second[i].value.first.payment_id == 500);
        EXPECT(updated->second[i].value.first.event_id == id);
        EXPECT(updated->second[i].value.second.url == "http://the_url");
        EXPECT(updated->second[i].value.second.token == "the_token");
        EXPECT(updated->second[i].value.second.confirmations == i + 1);

        EXPECT(updated->second[i].tx_info.link == outs[0].link);
        EXPECT(updated->second[i].tx_info.spend_meta.id == outs[0].spend_meta.id);
        EXPECT(updated->second[i].tx_info.pub == outs[0].pub);
        EXPECT(updated->second[i].tx_info.payment_id.short_ == outs[0].payment_id.short_);
      }
    }
  }
}
