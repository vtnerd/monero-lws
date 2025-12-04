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


#include "storage.test.h"

#include <boost/filesystem/operations.hpp>
#include "common/util.h"   // monero/src/
#include "db/string.h"
#include "error.h"
#include "framework.test.h"

namespace lws { namespace db { namespace test
{
  namespace
  {
    boost::filesystem::path get_db_location()
    {
      return tools::get_default_data_dir() + "light_wallet_server_unit_testing";
    }
  }

  cleanup_db::~cleanup_db()
  {
    boost::filesystem::remove_all(get_db_location());
  }

  storage get_fresh_db()
  {
    const boost::filesystem::path location = get_db_location();
    boost::filesystem::remove_all(location);
    boost::filesystem::create_directories(location);
    return storage::open(location.c_str(), 5);
  }

  db::account make_db_account(const account_address& pubs, const crypto::secret_key& key)
  {
    view_key converted_key{};
    std::memcpy(std::addressof(converted_key), std::addressof(unwrap(unwrap(key))), sizeof(key));
    return {
      account_id(1), account_time(0), pubs, converted_key
    };
  }

  lws::account make_account(const account_address& pubs, const crypto::secret_key& key)
  {
    return lws::account{make_db_account(pubs, key), {}, {}};
  }
}}} // lws // db // test

LWS_CASE("lws::db::storage")
{
  lws::db::account_address account_address{};
  crypto::secret_key view{};
  crypto::generate_keys(account_address.spend_public, view);
  crypto::generate_keys(account_address.view_public, view);
  const std::string address = lws::db::address_string(account_address);
  const std::string viewkey = epee::to_hex::string(epee::as_byte_span(unwrap(unwrap(view))));

  const lws::db::address_index lookahead{
    lws::db::major_index(2), lws::db::minor_index(2)
  };

  SETUP("Database with account")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    EXPECT(db.add_account(account_address, view));

    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());

    const auto get_account = [&db, &account_address] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account_address)).second;
    };

    SECTION("rollback lookahead_fail via rescan")
    {
      EXPECT(db.import_request(account_address, last_block.id, lookahead));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 10));
      
      const auto block_failed = lws::db::block_id(to_uint(last_block.id) + 1);
      const auto update =
        db.update_lookahead(account_address, block_failed, {lws::db::major_index(10), lws::db::minor_index(10)}, 10);
      EXPECT(update == -1);
      EXPECT(get_account().lookahead_fail == block_failed);

      EXPECT(db.rescan(last_block.id, {std::addressof(account_address), 1}));
      EXPECT(get_account().lookahead_fail == lws::db::block_id(0));
    }

    SECTION("rollback lookahead_fail via import")
    {
      EXPECT(db.import_request(account_address, last_block.id, lookahead));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 10));
      
      const auto block_failed = lws::db::block_id(to_uint(last_block.id) + 1);
      const auto update =
        db.update_lookahead(account_address, block_failed, {lws::db::major_index(10), lws::db::minor_index(10)}, 10);
      EXPECT(update == -1);
      EXPECT(get_account().lookahead_fail == block_failed);

      EXPECT(db.import_request(account_address, last_block.id, lookahead));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 10));
      EXPECT(get_account().lookahead_fail == lws::db::block_id(0));
    }

    const auto add_output = [&] ()
    {
      auto account = get_account();
      const lws::db::transaction_link link{
        lws::db::block_id(to_uint(last_block.id) + 1), crypto::rand<crypto::hash>()
      };
      const crypto::public_key tx_public = []() {
        crypto::secret_key secret;
        crypto::public_key out;
        crypto::generate_keys(out, secret);
        return out;
      }();
      const crypto::hash tx_prefix = crypto::rand<crypto::hash>();
      const crypto::public_key pub = crypto::rand<crypto::public_key>();
      const rct::key ringct = crypto::rand<rct::key>();
      const auto extra =
        lws::db::extra(lws::db::extra::coinbase_output | lws::db::extra::ringct_output);
      const auto payment_id_ = crypto::rand<lws::db::output::payment_id_>();
      const crypto::key_image image = crypto::rand<crypto::key_image>();

      lws::account real_account{account, {}, {}};
      real_account.add_out(
        lws::db::output{
          link,
          lws::db::output::spend_meta_{
            lws::db::output_id{500, 30},
            std::uint64_t(40000),
            std::uint32_t(16),
            std::uint32_t(2),
            tx_public
          },
          std::uint64_t(7000),
          std::uint64_t(4670),
          tx_prefix,
          pub,
          ringct,
          {0, 0, 0, 0, 0, 0, 0},
          lws::db::pack(extra, sizeof(crypto::hash)),
          payment_id_,
          std::uint64_t(100),
          lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(10)}
        }
      );
 
      {
        std::vector<crypto::hash> hashes{
          last_block.hash,
          crypto::rand<crypto::hash>(),
          crypto::rand<crypto::hash>(),
          crypto::rand<crypto::hash>(),
          crypto::rand<crypto::hash>(),
          crypto::rand<crypto::hash>()
        };

        const auto thing = db.update(last_block.id, epee::to_span(hashes), {std::addressof(real_account), 1}, {});
        if (!thing)
          std::cout << thing.error().message() << std::endl;
      }
    };


    SECTION("Lookahead with outputs")
    {
      add_output();
      const auto scan_height = get_account().scan_height;

      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))).empty());
      EXPECT(db.import_request(account_address, scan_height, lookahead));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 18));

      const std::vector<lws::db::subaddress_dict> expected_range{
        {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}},
        {lws::db::major_index(1), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}},
        {lws::db::major_index(2), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(11)}}}},
        {lws::db::major_index(3), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}},
      };
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))) == expected_range);

      SECTION("shrink lookahead")
      {
        const lws::db::address_index test1{
          lws::db::major_index(3), lws::db::minor_index(2)
        };
        const lws::db::address_index test2{
          lws::db::major_index(2), lws::db::minor_index(3)
        };
        const lws::db::address_index shrink{
          lws::db::major_index(1), lws::db::minor_index(1)
        };
        EXPECT(!db.shrink_lookahead(account_address, test1));
        EXPECT(!db.shrink_lookahead(account_address, test2));
        EXPECT(db.shrink_lookahead(account_address, lookahead));

        EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))) == expected_range);
        EXPECT(get_account().lookahead == lookahead);
        EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))) == expected_range);

        EXPECT(db.shrink_lookahead(account_address, shrink));
        EXPECT(get_account().lookahead == shrink);
        EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))) == expected_range);
      }
    }

    SECTION("Lookahead failure with outputs")
    {
      add_output();
      const auto scan_height = get_account().scan_height;

      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))).empty());
      EXPECT(db.import_request(account_address, scan_height, lookahead));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 17));

      const std::vector<lws::db::subaddress_dict> expected_range{
        {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}},
        {lws::db::major_index(1), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}},
        {lws::db::major_index(2), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(11)}}}}
      };

      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_subaddresses(lws::db::account_id(1))) == expected_range);

      SECTION("shrink lookahead")
      {
        const lws::db::address_index test1{
          lws::db::major_index(3), lws::db::minor_index(2)
        };
        const lws::db::address_index test2{
          lws::db::major_index(2), lws::db::minor_index(3)
        };
        const lws::db::address_index shrink{
          lws::db::major_index(1), lws::db::minor_index(1)
        };
        EXPECT(!db.shrink_lookahead(account_address, test1));
        EXPECT(!db.shrink_lookahead(account_address, test2));
        EXPECT(!db.shrink_lookahead(account_address, lookahead));
        EXPECT(!db.shrink_lookahead(account_address, shrink));
      }
    }
  }
}
