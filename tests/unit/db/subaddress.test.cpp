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

#include <boost/range/counting_range.hpp>
#include <boost/uuid/random_generator.hpp>
#include <cstdint>
#include "crypto/crypto.h" // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "db/storage.test.h"
#include "error.h"
#include "wire/error.h"

namespace
{
  struct user_account
  {
    lws::db::account_address account;
    crypto::secret_key view;

    user_account()
      : account{}, view{}
    {}
  };

  void check_address_map(lest::env& lest_env, lws::db::storage_reader& reader, const user_account& user, const std::vector<lws::db::subaddress_dict>& source)
  {
    SETUP("check_address_map")
    {
      lws::db::cursor::subaddress_indexes cur = nullptr;
      for (const auto& major_entry : source)
      {
        for (const auto& minor_entry : major_entry.second)
        {
          for (std::uint64_t elem : boost::counting_range(std::uint64_t(minor_entry[0]), std::uint64_t(minor_entry[1]) + 1))
          {
            const lws::db::address_index index{major_entry.first, lws::db::minor_index(elem)};
            auto result = reader.find_subaddress(lws::db::account_id(1), index.get_spend_public(user.account, user.view), cur);
            EXPECT(result.has_value());
            EXPECT(result == index);
          }
        }
      }
    }
  }
}

LWS_CASE("db::storage::upsert_subaddresses")
{
  user_account user{};
  crypto::generate_keys(user.account.spend_public, user.view);
  crypto::generate_keys(user.account.view_public, user.view);

  SETUP("One Account DB")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());
    MONERO_UNWRAP(db.add_account(user.account, user.view));

    SECTION("Empty get_subaddresses")
    {
      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))).empty());
    }

    SECTION("Upsert Basic")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(100)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      {
        lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());

        EXPECT(result.has_value());
        EXPECT(result->size() == 1);
        EXPECT(result->at(0).first == lws::db::major_index(0));
        EXPECT(result->at(0).second.size() == 1);
        EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
        EXPECT(result->at(0).second[0][1] == lws::db::minor_index(100));

        check_address_map(lest_env, reader, user, subs);
      }
      subs.back().first = lws::db::major_index(1);
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 199);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 1);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(100));
    }

    SECTION("Upsert Appended")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(100)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(100));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(101), lws::db::minor_index(200)}};
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 200);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(101));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(201), lws::db::minor_index(201)}};
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 200);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      auto reader = MONERO_UNWRAP(db.start_read());
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 1);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(200));
    }

    SECTION("Upsert Prepended")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(101), lws::db::minor_index(200)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(101));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(100)}};

      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 199);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 200);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(100));

      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      check_address_map(lest_env, reader, user, subs);

      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 1);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(200));
    }

    SECTION("Upsert Wrapped")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(101), lws::db::minor_index(200)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(101));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(300)}};
      
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 299);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 300);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 2);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(100));
      EXPECT(result->at(0).second[1][0] == lws::db::minor_index(201));
      EXPECT(result->at(0).second[1][1] == lws::db::minor_index(300));

      lws::db::storage_reader reader = MONERO_UNWRAP(db.start_read());
      check_address_map(lest_env, reader, user, subs);
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 1);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(300));
    }

    SECTION("Upsert After")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(100)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(100));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(102), lws::db::minor_index(200)}};
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 198);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 199);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(102));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      auto reader = MONERO_UNWRAP(db.start_read());
      check_address_map(lest_env, reader, user, subs);
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 2);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(100));
      EXPECT(fetched->at(0).second[1][0] == lws::db::minor_index(102));
      EXPECT(fetched->at(0).second[1][1] == lws::db::minor_index(200));
    }

    SECTION("Upsert Before")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(101), lws::db::minor_index(200)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(101));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(99)}};
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 198);
      EXPECT(result.has_error());
      EXPECT(result == lws::error::max_subaddresses);

      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 199);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(99));

      auto reader = MONERO_UNWRAP(db.start_read());
      check_address_map(lest_env, reader, user, subs);
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 2);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(99));
      EXPECT(fetched->at(0).second[1][0] == lws::db::minor_index(101));
      EXPECT(fetched->at(0).second[1][1] == lws::db::minor_index(200));
    }

    SECTION("Upsert Encapsulated")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(200)}}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 200);
      EXPECT(result.has_value());
      EXPECT(result->size() == 1);
      EXPECT(result->at(0).first == lws::db::major_index(0));
      EXPECT(result->at(0).second.size() == 1);
      EXPECT(result->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(result->at(0).second[0][1] == lws::db::minor_index(200));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        check_address_map(lest_env, reader, user, subs);
      }

      subs.back().second =
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(5), lws::db::minor_index(99)}};
      result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 300);
      EXPECT(result.has_value());
      EXPECT(result->size() == 0);

      auto reader = MONERO_UNWRAP(db.start_read());
      check_address_map(lest_env, reader, user, subs);
      const auto fetched = reader.get_subaddresses(lws::db::account_id(1));
      EXPECT(fetched.has_value());
      EXPECT(fetched->size() == 1);
      EXPECT(fetched->at(0).first == lws::db::major_index(0));
      EXPECT(fetched->at(0).second.size() == 1);
      EXPECT(fetched->at(0).second[0][0] == lws::db::minor_index(1));
      EXPECT(fetched->at(0).second[0][1] == lws::db::minor_index(200));
    }


    SECTION("Bad subaddress_dict")
    {
      std::vector<lws::db::subaddress_dict> subs{};
      subs.emplace_back(
        lws::db::major_index(0),
        lws::db::index_ranges{lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(100)}}
      );
      subs.back().second.push_back(
        lws::db::index_range{lws::db::minor_index(101), lws::db::minor_index(200)}
      );
      auto result = db.upsert_subaddresses(lws::db::account_id(1), user.account, user.view, subs, 100);
      EXPECT(result.has_error());
      EXPECT(result == wire::error::schema::array);
    }
  }
}
