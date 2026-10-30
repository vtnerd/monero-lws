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

#include "framework.test.h"

#include <algorithm>
#include <cstring>
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h"      // monero/src
#include "db/account.h"
#include "db/data.h"
#include "db/storage.h"
#include "db/storage.test.h"
#include "device/device_default.hpp"                  // monero/src
#include "mempool.h"
#include "util/account.test.h"
#include "util/transaction.test.h"

namespace
{
  
  lws::db::account make_account(const cryptonote::account_keys& src)
  {
    lws::db::account out{};
    std::memcpy(&out.address.spend_public, &src.m_account_address.m_spend_public_key, sizeof(out.address.spend_public));
    std::memcpy(&out.address.view_public, &src.m_account_address.m_view_public_key, sizeof(out.address.view_public));
    std::memcpy(&out.key, &unwrap(unwrap(src.m_view_secret_key)), sizeof(out.key));
    return out;
  }
}

LWS_CASE("mempool")
{
  const auto base = lws_test::make_account();

  SETUP("mempool")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    lws::mempool pool{};
    lws::account account{make_account(base), {}, {}};

    SECTION("empty")
    {
      auto reader = MONERO_UNWRAP(db.start_read());
      EXPECT(pool.scan_account(account, reader).empty());
    }

    SECTION("add_txs")
    {
      const auto other = lws_test::make_account();

      std::vector<cryptonote::tx_destination_entry> destinations;

      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = base.m_account_address;
      const auto tx1 = lws_test::make_tx(lest_env, base, destinations, 20, true);

      destinations.back().amount = 9000;
      destinations.back().addr = other.m_account_address;
      const auto tx2 = lws_test::make_tx(lest_env, other, destinations, 20, true);

      std::array<cryptonote::transaction, 2> txes{{tx1.tx, tx2.tx}};
      pool.add_txs(epee::to_mut_span(txes));

      auto reader = MONERO_UNWRAP(db.start_read());
      auto results = pool.scan_account(account, reader);
      EXPECT(results.size() == 1);
      EXPECT(results.at(0).hash == get_transaction_hash(tx1.tx));
      EXPECT(results.at(0).spends.size() == 0);
      EXPECT(results.at(0).outputs.size() == 1);
      EXPECT(results.at(0).outputs.at(0).link.height == lws::db::block_id::txpool);
      EXPECT(results.at(0).outputs.at(0).link.tx_hash == get_transaction_hash(tx1.tx));

      results = pool.scan_account(account, reader);
      EXPECT(results.size() == 1);
      EXPECT(results.at(0).hash == get_transaction_hash(tx1.tx));
      EXPECT(results.at(0).spends.size() == 0);
      EXPECT(results.at(0).outputs.size() == 1);
      EXPECT(results.at(0).outputs.at(0).link.height == lws::db::block_id::txpool);
      EXPECT(results.at(0).outputs.at(0).link.tx_hash == get_transaction_hash(tx1.tx));

      pool.reset_txs(lws::mempool::pool_table{});
      results = pool.scan_account(account, reader);
      EXPECT(results.empty());
    }

    SECTION("rest_txs")
    {
      const auto now = std::chrono::system_clock::now();
      const auto other = lws_test::make_account();

      std::vector<cryptonote::tx_destination_entry> destinations;

      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = base.m_account_address;
      const auto tx1 = lws_test::make_tx(lest_env, base, destinations, 20, true);

      destinations.back().amount = 9000;
      destinations.back().addr = other.m_account_address;
      const auto tx2 = lws_test::make_tx(lest_env, other, destinations, 20, true);

      pool.reset_txs(
        lws::mempool::pool_table{
          {get_transaction_hash(tx1.tx), std::make_shared<lws::mempool::pool_entry>(cryptonote::transaction{tx1.tx}, now)},
          {get_transaction_hash(tx2.tx), std::make_shared<lws::mempool::pool_entry>(cryptonote::transaction{tx2.tx}, now)}
        }
      );

      auto reader = MONERO_UNWRAP(db.start_read());
      auto results = pool.scan_account(account, reader);
      EXPECT(results.size() == 1);
      EXPECT(results.at(0).hash == get_transaction_hash(tx1.tx));
      EXPECT(results.at(0).spends.size() == 0);
      EXPECT(results.at(0).outputs.size() == 1);
      EXPECT(results.at(0).outputs.at(0).link.height == lws::db::block_id::txpool);
      EXPECT(results.at(0).outputs.at(0).link.tx_hash == get_transaction_hash(tx1.tx));
    }

    SECTION("subaddress")
    {
      const lws::db::account_address address{
        base.m_account_address.m_view_public_key,
        base.m_account_address.m_spend_public_key
      };
      EXPECT(db.add_account(address, base.m_view_secret_key));
      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
        EXPECT(db.upsert_subaddresses(lws::db::account_id(1), address, base.m_view_secret_key, indexes, 2));
      }

      hw::core::device_default hw{};
      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = base.m_account_address;
      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = hw.get_subaddress(base, cryptonote::subaddress_index{0, 1});
      destinations.back().is_subaddress = true;
      const auto tx = lws_test::make_tx(lest_env, base, destinations, 20, true);

      std::array<cryptonote::transaction, 1> txes{{tx.tx}};
      pool.add_txs(epee::to_mut_span(txes));

      lws::db::address_index lookahead{};
      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const lws::db::account db_account = MONERO_UNWRAP(reader.get_account(address)).second;
        lookahead = db_account.lookahead;
        lws::account full_account{db_account, {}, {}};

        const auto results = pool.scan_account(full_account, reader);
        EXPECT(results.size() == 1);
        EXPECT(results.at(0).hash == get_transaction_hash(tx.tx));
        EXPECT(results.at(0).outputs.size() == 2);

        const lws::db::address_index subaddress{lws::db::major_index::primary, lws::db::minor_index(1)};
        const auto& outputs = results.at(0).outputs;
        const auto to_subaddress = std::find_if(outputs.begin(), outputs.end(), [&] (const lws::db::output& out) {
          return out.recipient == subaddress;
        });
        EXPECT(to_subaddress != outputs.end());
        EXPECT(to_subaddress->spend_meta.amount == 2000u);
      }

      // The REST scan must not change the lookahead
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(address)).second.lookahead == lookahead);
    }

    SECTION("subaddress after skip")
    {
      // make_tx decodes all outputs with the view key of the sender, so the
      // scanned account uses the same view key with a different spend key
      cryptonote::account_keys other = lws_test::make_account();
      other.m_account_address.m_view_public_key = base.m_account_address.m_view_public_key;
      other.m_view_secret_key = base.m_view_secret_key;

      const lws::db::account_address address{
        other.m_account_address.m_view_public_key,
        other.m_account_address.m_spend_public_key
      };
      EXPECT(db.add_account(address, other.m_view_secret_key));

      hw::core::device_default hw{};
      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = base.m_account_address;
      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = hw.get_subaddress(other, cryptonote::subaddress_index{0, 1});
      destinations.back().is_subaddress = true;
      const auto tx = lws_test::make_tx(lest_env, base, destinations, 20, true);

      std::array<cryptonote::transaction, 1> txes{{tx.tx}};
      pool.add_txs(epee::to_mut_span(txes));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        lws::account full_account{MONERO_UNWRAP(reader.get_account(address)).second, {}, {}};
        EXPECT(pool.scan_account(full_account, reader).empty());
      }

      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
        EXPECT(db.upsert_subaddresses(lws::db::account_id(1), address, other.m_view_secret_key, indexes, 2));
      }

      auto reader = MONERO_UNWRAP(db.start_read());
      lws::account full_account{MONERO_UNWRAP(reader.get_account(address)).second, {}, {}};
      const auto results = pool.scan_account(full_account, reader);
      EXPECT(results.size() == 1);
      EXPECT(results.at(0).hash == get_transaction_hash(tx.tx));
      EXPECT(results.at(0).outputs.size() == 1);
      EXPECT(results.at(0).outputs.at(0).recipient == (lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}));
      EXPECT(results.at(0).outputs.at(0).spend_meta.amount == 2000u);
    }
  }
}
