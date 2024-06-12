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
  void add_spend(lws::account& account, const lws::db::block_id last_id)
  {
    account.add_spend(
      lws::db::spend{
        lws::db::transaction_link{
          lws::db::block_id(lmdb::to_native(last_id) + 1),
          crypto::rand<crypto::hash>()
        },
        crypto::rand<crypto::key_image>(),
        lws::db::output_id{0, 100},
        std::uint64_t(2000),
        std::uint64_t(0),
        std::uint32_t(16),
        {0, 0, 0},
        std::uint8_t(0),
        crypto::rand<crypto::hash>(),
        lws::db::address_index{lws::db::major_index(1), lws::db::minor_index(0)}
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

    const auto get_height = [&db] (const lws::db::block_id height) -> std::vector<lws::db::account_id>
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).accounts_at_height(height));
    };

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
      for (unsigned i = 0; i < 4; ++i)
      {
        crypto::hash chain[2] = {head.hash, crypto::rand<crypto::hash>()};

        auto updated = db.update(head.id, chain, {std::addressof(full_account), 1}, nullptr);
        EXPECT(!updated.has_error());
        EXPECT(updated->spend_pubs.empty());
        EXPECT(updated->accounts_updated == 1);
        if (i < 3)
        {
          EXPECT(updated->confirm_pubs.size() == 1);
          EXPECT(updated->confirm_pubs[0].key.user == lws::db::account_id(1));
          EXPECT(updated->confirm_pubs[0].key.type == lws::db::webhook_type::tx_confirmation);
          EXPECT(updated->confirm_pubs[0].value.first.payment_id == 500);
          EXPECT(updated->confirm_pubs[0].value.first.event_id == id);
          EXPECT(updated->confirm_pubs[0].value.second.url == "http://the_url");
          EXPECT(updated->confirm_pubs[0].value.second.token == "the_token");
          EXPECT(updated->confirm_pubs[0].value.second.confirmations == i + 1);

          EXPECT(updated->confirm_pubs[0].tx_info.link == outs[0].link);
          EXPECT(updated->confirm_pubs[0].tx_info.spend_meta.id == outs[0].spend_meta.id);
          EXPECT(updated->confirm_pubs[0].tx_info.pub == outs[0].pub);
          EXPECT(updated->confirm_pubs[0].tx_info.payment_id.short_ == outs[0].payment_id.short_);
        }
        else
          EXPECT(updated->confirm_pubs.empty());

        const auto next = lws::db::block_id(lmdb::to_native(head.id) + 1);
        full_account.updated(next);
        head = {next, chain[1]};
      }
    }

    SECTION("storage::update(...) all at once")
    {
      const crypto::hash chain[6] = {
        last_block.hash,
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>()
      };

      lws::account full_account = lws::db::test::make_account(account, view);
      full_account.updated(last_block.id);
      EXPECT(add_out(full_account, lws::db::block_id(lmdb::to_native(last_block.id) + 1), 500));

      const std::vector<lws::db::output> outs = full_account.outputs();
      EXPECT(outs.size() == 1);

      const auto updated = db.update(last_block.id, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(!updated.has_error());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 3);

      for (unsigned i = 0; i < 3; ++i)
      {
        EXPECT(updated->confirm_pubs[i].key.user == lws::db::account_id(1));
        EXPECT(updated->confirm_pubs[i].key.type == lws::db::webhook_type::tx_confirmation);
        EXPECT(updated->confirm_pubs[i].value.first.payment_id == 500);
        EXPECT(updated->confirm_pubs[i].value.first.event_id == id);
        EXPECT(updated->confirm_pubs[i].value.second.url == "http://the_url");
        EXPECT(updated->confirm_pubs[i].value.second.token == "the_token");
        EXPECT(updated->confirm_pubs[i].value.second.confirmations == i + 1);

        EXPECT(updated->confirm_pubs[i].tx_info.link == outs[0].link);
        EXPECT(updated->confirm_pubs[i].tx_info.spend_meta.id == outs[0].spend_meta.id);
        EXPECT(updated->confirm_pubs[i].tx_info.pub == outs[0].pub);
        EXPECT(updated->confirm_pubs[i].tx_info.payment_id.short_ == outs[0].payment_id.short_);
      }
    }

    SECTION("Skip Already Scanned/Published")
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
      EXPECT(add_out(full_account, lws::db::block_id(lmdb::to_native(last_block.id) + 1), 500));

      const std::vector<lws::db::output> outs = full_account.outputs();
      EXPECT(outs.size() == 1);

      auto updated = db.update(last_block.id, {chain, 3}, {std::addressof(full_account), 1}, nullptr);
      EXPECT(updated.has_value());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 1);

      EXPECT(updated->confirm_pubs[0].key.user == lws::db::account_id(1));
      EXPECT(updated->confirm_pubs[0].key.type == lws::db::webhook_type::tx_confirmation);
      EXPECT(updated->confirm_pubs[0].value.first.payment_id == 500);
      EXPECT(updated->confirm_pubs[0].value.first.event_id == id);
      EXPECT(updated->confirm_pubs[0].value.second.url == "http://the_url");
      EXPECT(updated->confirm_pubs[0].value.second.token == "the_token");
      EXPECT(updated->confirm_pubs[0].value.second.confirmations == 1);

      EXPECT(updated->confirm_pubs[0].tx_info.link == outs[0].link);
      EXPECT(updated->confirm_pubs[0].tx_info.spend_meta.id == outs[0].spend_meta.id);
      EXPECT(updated->confirm_pubs[0].tx_info.pub == outs[0].pub);
      EXPECT(updated->confirm_pubs[0].tx_info.payment_id.short_ == outs[0].payment_id.short_);

      full_account.updated(lws::db::block_id(lmdb::to_native(last_block.id) + 2));
      EXPECT(full_account.outputs().empty());

      updated = db.update(last_block.id, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(updated.has_value());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 2);

      for (unsigned i = 0; i < 2; ++i)
      {
        EXPECT(updated->confirm_pubs[i].key.user == lws::db::account_id(1));
        EXPECT(updated->confirm_pubs[i].key.type == lws::db::webhook_type::tx_confirmation);
        EXPECT(updated->confirm_pubs[i].value.first.payment_id == 500);
        EXPECT(updated->confirm_pubs[i].value.first.event_id == id);
        EXPECT(updated->confirm_pubs[i].value.second.url == "http://the_url");
        EXPECT(updated->confirm_pubs[i].value.second.token == "the_token");
        EXPECT(updated->confirm_pubs[i].value.second.confirmations == i + 2);

        EXPECT(updated->confirm_pubs[i].tx_info.link == outs[0].link);
        EXPECT(updated->confirm_pubs[i].tx_info.spend_meta.id == outs[0].spend_meta.id);
        EXPECT(updated->confirm_pubs[i].tx_info.pub == outs[0].pub);
        EXPECT(updated->confirm_pubs[i].tx_info.payment_id.short_ == outs[0].payment_id.short_);
      }
    }

    SECTION("rescan with existing event")
    {
      const auto scan_height = lws::db::block_id(lmdb::to_native(last_block.id) + 1);
      crypto::hash chain[2] = {
        last_block.hash,
        crypto::rand<crypto::hash>()
      };

      lws::account full_account = lws::db::test::make_account(account, view);
      full_account.updated(last_block.id);
      EXPECT(add_out(full_account, last_block.id, 500));

      const std::vector<lws::db::output> outs = full_account.outputs();
      EXPECT(outs.size() == 1);

      auto updated = db.update(last_block.id, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(updated.has_value());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 1);

      EXPECT(updated->confirm_pubs[0].key.user == lws::db::account_id(1));
      EXPECT(updated->confirm_pubs[0].key.type == lws::db::webhook_type::tx_confirmation);
      EXPECT(updated->confirm_pubs[0].value.first.payment_id == 500);
      EXPECT(updated->confirm_pubs[0].value.first.event_id == id);
      EXPECT(updated->confirm_pubs[0].value.second.url == "http://the_url");
      EXPECT(updated->confirm_pubs[0].value.second.token == "the_token");
      EXPECT(updated->confirm_pubs[0].value.second.confirmations == 1);

      EXPECT(updated->confirm_pubs[0].tx_info.link == outs[0].link);
      EXPECT(updated->confirm_pubs[0].tx_info.spend_meta.id == outs[0].spend_meta.id);
      EXPECT(updated->confirm_pubs[0].tx_info.pub == outs[0].pub);
      EXPECT(updated->confirm_pubs[0].tx_info.payment_id.short_ == outs[0].payment_id.short_);

      full_account.updated(scan_height);
      EXPECT(full_account.scan_height() == scan_height);
      EXPECT(get_height(last_block.id).empty());
      auto height = get_height(scan_height);
      EXPECT(height.size() == 1);
      EXPECT(height[0] == lws::db::account_id(1));

      updated = db.update(last_block.id, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(updated.has_value());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->confirm_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(get_height(last_block.id).empty());
      height = get_height(scan_height);
      EXPECT(height.size() == 1);
      EXPECT(height[0] == lws::db::account_id(1));

      // issue a rescan, and ensure that hooks are not triggered
      const std::size_t chain_size = std::end(chain) - std::begin(chain);
      const auto new_height = lws::db::block_id(lmdb::to_native(last_block.id) - chain_size);
      const auto rescanned =
        db.rescan(new_height, {std::addressof(full_account.db_address()), 1});
      EXPECT(rescanned.has_value());
      EXPECT(rescanned->size() == 1);

      EXPECT(get_height(last_block.id).empty());
      EXPECT(get_height(scan_height).empty());
      height = get_height(new_height);
      EXPECT(height.size() == 1);
      EXPECT(height[0] == lws::db::account_id(1));

      full_account.updated(new_height);
      EXPECT(full_account.scan_height() == scan_height);

      full_account = lws::db::test::make_account(account, view);
      full_account.updated(new_height);
      EXPECT(full_account.scan_height() == new_height);
      updated = db.update(new_height, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(updated.has_value());
      EXPECT(updated->spend_pubs.empty());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 0);

      EXPECT(get_height(last_block.id).empty());
      EXPECT(get_height(scan_height).empty());
      EXPECT(get_height(new_height).empty());
      height = get_height(lws::db::block_id(lmdb::to_native(new_height) + 1));
      EXPECT(height.size() == 1);
      EXPECT(height[0] == lws::db::account_id(1));
    }

    SECTION("Add db spend")
    {
      const boost::uuids::uuid other_id = boost::uuids::random_generator{}();
      {
        lws::db::webhook_value value{
          lws::db::webhook_dupsort{0, other_id},
          lws::db::webhook_data{"http://the_url_spend", "the_token_spend"}
        };
        MONERO_UNWRAP(
          db.add_webhook(lws::db::webhook_type::tx_spend, account, std::move(value))
        );
      }
      SECTION("storage::get_webhooks()")
      {
        lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
        const auto result = MONERO_UNWRAP(reader.get_webhooks());
        EXPECT(result.size() == 2);
        EXPECT(result[0].first.user == lws::db::account_id(1));
        EXPECT(result[0].first.type == lws::db::webhook_type::tx_confirmation);
        EXPECT(result[0].second.size() == 1);
        EXPECT(result[0].second[0].first.payment_id == 500);
        EXPECT(result[0].second[0].first.event_id == id);
        EXPECT(result[0].second[0].second.url == "http://the_url");
        EXPECT(result[0].second[0].second.token == "the_token");
        EXPECT(result[0].second[0].second.confirmations == 3);

        EXPECT(result[1].first.user == lws::db::account_id(1));
        EXPECT(result[1].first.type == lws::db::webhook_type::tx_spend);
        EXPECT(result[1].second.size() == 1);
        EXPECT(result[1].second[0].first.payment_id == 0);
        EXPECT(result[1].second[0].first.event_id == other_id);
        EXPECT(result[1].second[0].second.url == "http://the_url_spend");
        EXPECT(result[1].second[0].second.token == "the_token_spend");
        EXPECT(result[1].second[0].second.confirmations == 0);
      }
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
      add_spend(full_account, last_block.id);
      const auto outs = full_account.outputs();
      const auto spends = full_account.spends();
      EXPECT(outs.size() == 1);
      EXPECT(spends.size() == 1);

      const auto updated = db.update(last_block.id, chain, {std::addressof(full_account), 1}, nullptr);
      EXPECT(!updated.has_error());
      EXPECT(updated->accounts_updated == 1);
      EXPECT(updated->confirm_pubs.size() == 3);
      EXPECT(updated->spend_pubs.size() == 1);

      EXPECT(updated->spend_pubs[0].key.user == lws::db::account_id(1));
      EXPECT(updated->spend_pubs[0].key.type == lws::db::webhook_type::tx_spend);
      EXPECT(updated->spend_pubs[0].value.first.payment_id == 0);
      EXPECT(updated->spend_pubs[0].value.first.event_id == other_id);
      EXPECT(updated->spend_pubs[0].value.second.url == "http://the_url_spend");
      EXPECT(updated->spend_pubs[0].value.second.token == "the_token_spend");
      EXPECT(updated->spend_pubs[0].value.second.confirmations == 0);

      EXPECT(updated->spend_pubs[0].tx_info.input.link == spends[0].link);
      EXPECT(updated->spend_pubs[0].tx_info.input.image == spends[0].image);
      EXPECT(updated->spend_pubs[0].tx_info.input.timestamp == spends[0].timestamp);
      EXPECT(updated->spend_pubs[0].tx_info.input.unlock_time == spends[0].unlock_time);
      EXPECT(updated->spend_pubs[0].tx_info.input.mixin_count == spends[0].mixin_count);
      EXPECT(updated->spend_pubs[0].tx_info.input.length == spends[0].length);
      EXPECT(updated->spend_pubs[0].tx_info.input.payment_id == spends[0].payment_id);
      EXPECT(updated->spend_pubs[0].tx_info.input.sender.maj_i == lws::db::major_index(1));
      EXPECT(updated->spend_pubs[0].tx_info.input.sender.min_i == lws::db::minor_index(0));

      EXPECT(updated->spend_pubs[0].tx_info.source.id == outs[0].spend_meta.id);
      EXPECT(updated->spend_pubs[0].tx_info.source.amount == outs[0].spend_meta.amount);
      EXPECT(updated->spend_pubs[0].tx_info.source.mixin_count == outs[0].spend_meta.mixin_count);
      EXPECT(updated->spend_pubs[0].tx_info.source.index == outs[0].spend_meta.index);
      EXPECT(updated->spend_pubs[0].tx_info.source.tx_public == outs[0].spend_meta.tx_public);
    }
  }
}
