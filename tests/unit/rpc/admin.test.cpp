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

#include <boost/range/algorithm/equal.hpp>
#include "db/storage.test.h"
#include "db/string.h"
#include "error.h"
#include "hex.h" // monero/contrib/epee/include
#include "rpc/admin.h"
#include "wire/json.h"

namespace
{
  constexpr const char address_str[] =
    u8"42ui2zRV3KBKgHPnQHDZu7WFc397XmhEjL9e6UnSpyHiKh4vydo7atvaQDSDKYPoCb51GQZc7hZZvDrJM7JCyuYqHHbshVn";
  constexpr const char view_str[] =
    u8"9ec001644f8d79ecb368083e48e7efb5a48b3563c9a78ba497874fd58285330d";

  template<typename T>
  expect<epee::byte_slice> call_endpoint(lws::db::storage disk, std::string json)
  {
    using request_type = typename T::request;
    expect<request_type> req = wire::json::from_bytes<request_type>(std::move(json));
    if (!req)
      return req.error();
    wire::json_slice_writer out{};
    MONERO_CHECK(T{}(out, std::move(disk), std::move(*req)));
    return out.take_bytes();
  }
}

LWS_CASE("rpc::admin")
{
  lws::db::account_address account = MONERO_UNWRAP(lws::db::address_string(address_str));
  crypto::secret_key view{};
  EXPECT(epee::from_hex::to_buffer(epee::as_mut_byte_span(unwrap(unwrap(view))), view_str));

  SETUP("One Account One Webhook Database")
  {
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());
    MONERO_UNWRAP(db.add_account(account, view));

    boost::uuids::uuid id{};
    epee::byte_slice id_str{};
    expect<epee::byte_slice> result{lws::error::configuration};
    {
      std::string add_json_str{};
      add_json_str.append(u8"{\"url\":\"http://the_url\", \"token\":\"the_token\",");
      add_json_str.append(u8"\"address\":\"").append(address_str).append(u8"\",");
      add_json_str.append(u8"\"payment_id\":\"deadbeefdeadbeef\",");
      add_json_str.append(u8"\"type\":\"tx-confirmation\",\"confirmations\":3}");
      result = call_endpoint<lws::rpc::webhook_add_>(db.clone(), std::move(add_json_str));
      EXPECT(!result.has_error());
    }

    {
      static constexpr const char begin[] =
        u8"{\"payment_id\":\"deadbeefdeadbeef\",\"event_id\":\"";
      epee::byte_slice begin_ = result->take_slice(sizeof(begin) - 1);
      EXPECT(boost::range::equal(std::string{begin}, begin_));
    }
    {
      id_str = result->take_slice(32);
      const boost::string_ref id_hex{
        reinterpret_cast<const char*>(id_str.data()), id_str.size()
      };
      EXPECT(epee::from_hex::to_buffer(epee::as_mut_byte_span(id), id_hex));
    }

    SECTION("webhook_add")
    {
      static constexpr const char end[] =
        u8"\",\"token\":\"the_token\",\"confirmations\":3,\"url\":\"http://the_url\"}";
      EXPECT(boost::range::equal(std::string{end}, *result));
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).size() == 1);
    }

    SECTION("webhook_delete_uuid")
    {
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).size() == 1);
      std::string delete_json_str{};
      delete_json_str.append(u8"{\"addresses\":[\"");
      delete_json_str.append(address_str);
      delete_json_str.append(u8"\"]}");

      expect<epee::byte_slice> result2 =
        call_endpoint<lws::rpc::webhook_delete_>(db.clone(), std::move(delete_json_str));
      EXPECT(!result2.has_error());
      EXPECT(boost::range::equal(std::string{u8"{}"}, *result2));
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).empty());
    }

    SECTION("webhook_delete_uuid")
    {
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).size() == 1);
      std::string delete_json_str{};
      delete_json_str.append(u8"{\"event_ids\":[\"");
      delete_json_str.append(reinterpret_cast<const char*>(id_str.data()), id_str.size());
      delete_json_str.append(u8"\"]}");

      expect<epee::byte_slice> result2 =
        call_endpoint<lws::rpc::webhook_del_uuid_>(db.clone(), std::move(delete_json_str));
      EXPECT(!result2.has_error());
      EXPECT(boost::range::equal(std::string{u8"{}"}, *result2));
      EXPECT(MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_webhooks()).empty());
    }

    SECTION("webhook_list")
    {
      wire::json_slice_writer out{};
      EXPECT(lws::rpc::webhook_list(out, db.clone()));
      expect<epee::byte_slice> bytes = out.take_bytes();
      EXPECT(!bytes.has_error());

      {
        static constexpr const char begin[] =
          u8"{\"webhooks\":[{\"key\":{\"user\":1,\"type\":\"tx-confirmation\"}"
            ",\"value\":[{\"payment_id\":\"deadbeefdeadbeef\",\"event_id\":\"";
        epee::byte_slice begin_ = bytes->take_slice(sizeof(begin) - 1);
        EXPECT(boost::range::equal(std::string{begin}, begin_));
      }
      {
        boost::uuids::uuid id_{};
        epee::byte_slice id_str_ = bytes->take_slice(32);
        const boost::string_ref id_hex{
          reinterpret_cast<const char*>(id_str_.data()), id_str_.size()
        };
        EXPECT(epee::from_hex::to_buffer(epee::as_mut_byte_span(id_), id_hex));
        EXPECT(id_ == id);
      }
      {
        static constexpr const char end[] =
          u8"\",\"token\":\"the_token\",\"confirmations\":3,\"url\":\"http://the_url\"}]}]}";
        EXPECT(boost::range::equal(std::string{end}, *bytes));
      }
    }
  }
}
