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

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket/error.hpp>
#include <memory>
#include <optional>

#include "db/data.h"
#include "db/print.test.h"
#include "db/storage.test.h"
#include "db/string.h"
#include "error.h"
#include "hex.h" // monero/epee/contrib/include
#include "net/http_client.h"
#include "rapidjson/document.h"     // monero/external/rapidjson/include
#include "rapidjson/stringbuffer.h" // monero/external/rapidjson/incldue
#include "rapidjson/prettywriter.h" // monero/external/rapidjson/incldue
#include "rest_server.h"
#include "scanner.test.h"
#include "rpc/pull.test.h"

#include "misc_log_ex.h"

namespace rapidjson
{
  std::ostream& operator<<(std::ostream& out, const Document& src)
  {
    StringBuffer buffer;
    PrettyWriter<StringBuffer> writer{buffer};
    src.Accept(writer);

    return out << buffer.GetString();
  }
}

namespace
{
  namespace enet = epee::net_utils;

  constexpr const char fake_daemon[] = "inproc://fake_daemon";
  constexpr const char rest_server[] = "http://127.0.0.1:10000";
  constexpr const char admin_server[] = "http://127.0.0.1:10001";

  struct join
  {
      boost::thread& thread;
      ~join() { thread.join(); }
  };

  struct rct_bytes
  {
    rct::key commitment;
    rct::key mask;
    rct::key amount;
  };

  std::pair<std::string, unsigned> invoke_base(enet::http::http_simple_client& client, const boost::string_ref uri, const boost::string_ref body)
  {
    const enet::http::http_response_info* info = nullptr;
    if (!client.invoke(uri, "POST", body, std::chrono::milliseconds{500}, std::addressof(info), {}))
      throw std::runtime_error{"HTTP invoke failed"};
    return {std::string{info->m_body}, info->m_response_code}; 
  }

  std::string invoke(enet::http::http_simple_client& client, const boost::string_ref uri, const boost::string_ref body)
  {
    auto result = invoke_base(client, uri, body);
    if (result.second != 200)
      throw std::runtime_error{"HTTP invoke not 200, instead " + std::to_string(result.second)};
    return result.first;
  }

  epee::byte_slice get_fee_response()
  {
    return epee::byte_slice{
      std::string{
        "{\"jsonrpc\":2.0,\"id\":0,\"result\":{"
          "\"estimated_base_fee\":10000,\"fee_mask\":1000,\"size_scale\":256,\"hard_fork_version\":16,\"fees\":[40,41]"
        "}}"
      }
    };
  }

  rct_bytes get_rct_bytes(const crypto::secret_key& user_key, const crypto::public_key& tx_public, const rct::key& ringct_mask, const std::uint64_t amount, const std::uint32_t index, bool coinbase)
  {
    rct_bytes out{};

    if (!coinbase)
    {
      crypto::key_derivation derived;
      if (!crypto::generate_key_derivation(tx_public, user_key, derived))
        MONERO_THROW(lws::error::crypto_failure, "generate_key_derivation failed");

      crypto::secret_key scalar;
      rct::ecdhTuple encrypted{ringct_mask, rct::d2h(amount)};

      crypto::derivation_to_scalar(derived, index, scalar);
      rct::ecdhEncode(encrypted, rct::sk2rct(scalar), false);

      out.commitment = rct::commit(amount, ringct_mask);
      out.mask = encrypted.mask;
      out.amount = encrypted.amount;
    }
    else
      out.mask = rct::identity();

    return out;
  }

  void verify_json(lest::env& lest_env, std::string name, std::string actual_json, std::string expected_json)
  {
    const lest::ctx ctx{lest_env, std::move(name)};

    rapidjson::Document expected;
    expected.Parse(expected_json.c_str());
    EXPECT(!expected.HasParseError());

    rapidjson::Document actual;
    actual.Parse(actual_json.c_str());
    EXPECT(!actual.HasParseError());

    EXPECT(expected == actual);
  }

  std::string get_prefix(const std::string& raw)
  {
    char const* const sep = std::strchr(raw.c_str(), u8':');
    if (!sep)
      return {};
    return {raw.data(), std::size_t(sep - raw.data() + 1)};
  }
}

LWS_CASE("rest_server")
{
  lws::db::account_address account_address{};
  crypto::secret_key view{};
  crypto::generate_keys(account_address.spend_public, view);
  crypto::generate_keys(account_address.view_public, view);
  const std::string address = lws::db::address_string(account_address);
  const std::string viewkey = epee::to_hex::string(epee::as_byte_span(unwrap(unwrap(view))));

  SETUP("Database and login")
  {
    std::optional<lws::rest_server> server;
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    auto context =
      lws::rpc::context::make(lws_test::rpc_rendevous, {}, {}, {}, std::chrono::minutes{0}, false, true);
    const auto rpc = MONERO_UNWRAP(context.connect());
    {
      const lws::rest_server::configuration config{
        {}, {}, std::chrono::seconds{10}, 1, 20, {}, false, true, true, false
      };
      std::vector<std::string> addresses{rest_server};
      server.emplace(
        epee::to_span(addresses),
        std::vector<std::string>{admin_server},
        db.clone(),
        MONERO_UNWRAP(rpc.clone()),
        config
      );
    }

    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());
    const auto get_account = [&db, &account_address] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account_address)).second;
    };

    const auto get_full_account = [&db, &get_account] () -> lws::account
    {
      const lws::db::account acct = get_account();
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_full_account(acct));
    };


    enet::http::http_simple_client client{};
    client.set_server("127.0.0.1", "10000", boost::none);
    EXPECT(client.connect(std::chrono::milliseconds{500})); 

    std::string message =
      "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"create_account\":true,\"generated_locally\":true}";
    std::string response = invoke(client, "/login", message);
    EXPECT(response == "{\"new_address\":true,\"generated_locally\":true,\"lookahead\":{\"maj_i\":0,\"min_i\":0}}");

    auto account = get_account();
    EXPECT(account.id == lws::db::account_id(1));

    SECTION("Empty Account")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height));
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";
      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      response = invoke(client, "/get_address_txs", message);
      EXPECT(response ==
        "{\"total_received\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      std::vector<epee::byte_slice> messages;
      messages.emplace_back(get_fee_response());
      boost::thread server_thread(&lws_test::rpc_thread, context.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"amount\":\"0\"}";
      response = invoke(client, "/get_unspent_outs", message);
      EXPECT(response == "{\"per_byte_fee\":39,\"fee_mask\":1000,\"amount\":\"0\",\"fees\":[40,41]}");
    }

    SECTION("Import from height")
    {
      EXPECT(account.start_height != lws::db::block_id(0));

      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height));
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      const std::string import_height = std::to_string(std::uint64_t(account.start_height) - 1);
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";
      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\", \"from_height\":" + import_height + "}";
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Accepted, waiting for approval\","
        "\"new_request\":true,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 0));
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Approved\","
        "\"new_request\":false,"
        "\"request_fulfilled\":true,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + import_height + "," +
        "\"scanned_block_height\":" + import_height + ","
        "\"start_height\":" + import_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );
    }

    SECTION("Import with lookahead")
    {
      EXPECT(account.start_height != lws::db::block_id(0));

      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height));
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";
      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\", \"lookahead\":{\"maj_i\":2,\"min_i\":3}}";
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Accepted, waiting for approval\","
        "\"new_request\":true,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Waiting for Approval\","
        "\"new_request\":false,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{};
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 6));
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Approved\","
        "\"new_request\":false,"
        "\"request_fulfilled\":true,"
        "\"lookahead\":{\"maj_i\":2,\"min_i\":3}}"
      );

      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":0,"
        "\"scanned_block_height\":0,"
        "\"start_height\":0,"
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"lookahead\":{\"maj_i\":2,\"min_i\":3}}"
      );

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const auto account = MONERO_UNWRAP(reader.get_account(lws::db::account_status::active, lws::db::account_id(1)));
        const lws::db::address_index lookahead{lws::db::major_index(2), lws::db::minor_index(3)};
        EXPECT(account.lookahead == lookahead);
        EXPECT(account.lookahead_fail == lws::db::block_id(0));
      
        const std::vector<lws::db::subaddress_dict> expected_range{
          {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(2)}}}},
          {lws::db::major_index(1), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(2)}}}},
        };
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }
    }

    SECTION("Import with lookahead failure")
    {
      EXPECT(account.start_height != lws::db::block_id(0));

      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height));
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";
      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\", \"lookahead\":{\"maj_i\":2,\"min_i\":3}}";
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Accepted, waiting for approval\","
        "\"new_request\":true,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Waiting for Approval\","
        "\"new_request\":false,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":0,\"min_i\":0}}"
      );

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{};
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account_address), 1}, 5));
      response = invoke(client, "/import_wallet_request", message);
      EXPECT(response ==
        "{\"import_fee\":\"0\","
        "\"status\":\"Accepted, waiting for approval\","
        "\"new_request\":true,"
        "\"request_fulfilled\":false,"
        "\"lookahead\":{\"maj_i\":2,\"min_i\":3}}"
      );

      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"0\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":0,"
        "\"scanned_block_height\":0,"
        "\"start_height\":0,"
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"lookahead_fail\":1,"
        "\"lookahead\":{\"maj_i\":2,\"min_i\":3}}"
      );

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const auto account = MONERO_UNWRAP(reader.get_account(lws::db::account_status::active, lws::db::account_id(1)));
        const lws::db::address_index lookahead{lws::db::major_index(2), lws::db::minor_index(3)};
        EXPECT(account.lookahead == lookahead);
        EXPECT(account.lookahead_fail == lws::db::block_id(1));
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))).empty());
      }
    }


    SECTION("One Receive, Zero Spends")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";

      const lws::db::transaction_link link{
        lws::db::block_id(4000), crypto::rand<crypto::hash>()
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
          lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(66)}
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

        EXPECT(db.update(last_block.id, epee::to_span(hashes), {std::addressof(real_account), 1}, {}));
      }

      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"40000\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + "}"
      );

      response = invoke(client, "/get_address_txs", message);
      EXPECT(response ==
        "{\"total_received\":\"40000\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"transactions\":["
          "{\"id\":0,"
          "\"hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"timestamp\":\"1970-01-01T01:56:40Z\","
          "\"total_received\":\"40000\","
          "\"total_sent\":\"0\","
          "\"fee\":\"100\","
          "\"unlock_time\":4670,"
          "\"height\":4000,"
          "\"payment_id\":\"" + epee::to_hex::string(epee::as_byte_span(payment_id_.long_)) + "\","
          "\"coinbase\":true,"
          "\"mempool\":false,"
          "\"mixin\":16,"
          "\"recipient\":{\"maj_i\":2,\"min_i\":66}}"
        "]}"
      );

      std::vector<epee::byte_slice> messages;
      messages.emplace_back(get_fee_response());
      boost::thread server_thread(&lws_test::rpc_thread, context.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};

      const auto ringct_expanded = get_rct_bytes(view, tx_public, ringct, 40000, 2, true);
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"amount\":\"0\"}";
      response = invoke(client, "/get_unspent_outs", message);
      EXPECT(response ==
        "{\"per_byte_fee\":39,"
        "\"fee_mask\":1000,"
        "\"amount\":\"40000\","
        "\"outputs\":["
          "{\"amount\":\"40000\","
          "\"public_key\":\"" + epee::to_hex::string(epee::as_byte_span(pub)) + "\","
          "\"index\":2,"
          "\"global_index\":30,"
          "\"tx_id\":30,"
          "\"tx_hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"tx_prefix_hash\":\"" + epee::to_hex::string(epee::as_byte_span(tx_prefix)) + "\","
          "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(tx_public)) + "\","
          "\"timestamp\":\"1970-01-01T01:56:40Z\","
          "\"height\":4000,"
          "\"rct\":\"" + epee::to_hex::string(epee::as_byte_span(ringct_expanded)) + "\","
          "\"recipient\":{\"maj_i\":2,\"min_i\":66}}"
        "],\"fees\":[40,41]}"
      );
    }

    SECTION("One Receive, One Spend")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";

      const lws::db::transaction_link link{
        lws::db::block_id(4000), crypto::rand<crypto::hash>()
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
      const crypto::hash payment_id = crypto::rand<crypto::hash>();
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
          lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(66)}
        }
      );
      real_account.add_spend(
        lws::db::spend{
          link,
          image,
          lws::db::output_id{500, 30},
          std::uint64_t(66),
          std::uint64_t(1500),
          std::uint32_t(16),
          {0, 0, 0},
          32,
          payment_id,
          lws::db::address_index{lws::db::major_index(4), lws::db::minor_index(55)}     
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

        EXPECT(db.update(last_block.id, epee::to_span(hashes), {std::addressof(real_account), 1}, {}));
      }

      response = invoke(client, "/get_address_info", message);
      EXPECT(response ==
        "{\"locked_funds\":\"0\","
        "\"total_received\":\"40000\","
        "\"total_sent\":\"40000\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"spent_outputs\":[{"
          "\"amount\":\"40000\","
          "\"key_image\":\"" + epee::to_hex::string(epee::as_byte_span(image)) + "\","
          "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(tx_public)) + "\","
          "\"out_index\":2,"
          "\"mixin\":16,"
          "\"sender\":{\"maj_i\":4,\"min_i\":55}"
        "}]}"
      );

      response = invoke(client, "/get_address_txs", message);
      EXPECT(response ==
        "{\"total_received\":\"40000\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"transactions\":["
          "{\"id\":0,"
          "\"hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"timestamp\":\"1970-01-01T01:56:40Z\","
          "\"total_received\":\"40000\","
          "\"total_sent\":\"40000\","
          "\"fee\":\"100\","
          "\"unlock_time\":4670,"
          "\"height\":4000,"
          "\"payment_id\":\"" + epee::to_hex::string(epee::as_byte_span(payment_id_.long_)) + "\","
          "\"coinbase\":true,"
          "\"mempool\":false,"
          "\"mixin\":16,"
          "\"recipient\":{\"maj_i\":2,\"min_i\":66},"
          "\"spent_outputs\":[{"
            "\"amount\":\"40000\","
            "\"key_image\":\"" + epee::to_hex::string(epee::as_byte_span(image)) + "\","
            "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(tx_public)) + "\","
            "\"out_index\":2,"
            "\"mixin\":16,"
            "\"sender\":{\"maj_i\":4,\"min_i\":55}"
          "}]}"
        "]}"
      );

      std::vector<epee::byte_slice> messages;
      messages.emplace_back(get_fee_response());
      boost::thread server_thread(&lws_test::rpc_thread, context.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};

      const auto ringct_expanded = get_rct_bytes(view, tx_public, ringct, 40000, 2, true);
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"amount\":\"0\"}";
      response = invoke(client, "/get_unspent_outs", message);
      EXPECT(response ==
        "{\"per_byte_fee\":39,"
        "\"fee_mask\":1000,"
        "\"amount\":\"40000\","
        "\"outputs\":["
          "{\"amount\":\"40000\","
          "\"public_key\":\"" + epee::to_hex::string(epee::as_byte_span(pub)) + "\","
          "\"index\":2,"
          "\"global_index\":30,"
          "\"tx_id\":30,"
          "\"tx_hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"tx_prefix_hash\":\"" + epee::to_hex::string(epee::as_byte_span(tx_prefix)) + "\","
          "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(tx_public)) + "\","
          "\"timestamp\":\"1970-01-01T01:56:40Z\","
          "\"height\":4000,"
          "\"spend_key_images\":[\"" + epee::to_hex::string(epee::as_byte_span(image)) + "\"],"
          "\"rct\":\"" + epee::to_hex::string(epee::as_byte_span(ringct_expanded)) + "\","
          "\"recipient\":{\"maj_i\":2,\"min_i\":66}}"
        "],\"fees\":[40,41]}"
      );
    }

    SECTION("provision_subaddrs")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"maj_i\":0,\"min_i\":0,\"n_maj\":2,\"n_min\":5}";

      response = invoke(client, "/provision_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[0,4]]},"
          "{\"key\":1,\"value\":[[0,4]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[0,4]]},"
          "{\"key\":1,\"value\":[[0,4]]}]}"
      );

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"maj_i\":2,\"min_i\":5,\"n_maj\":2,\"n_min\":5}";
      response = invoke(client, "/provision_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":2,\"value\":[[5,9]]},"
          "{\"key\":3,\"value\":[[5,9]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[0,4]]},"
          "{\"key\":1,\"value\":[[0,4]]},"
          "{\"key\":2,\"value\":[[5,9]]},"
          "{\"key\":3,\"value\":[[5,9]]}]}"
      );
    }

    SECTION("upsert_subaddrs")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"subaddrs\":[{\"key\":0,\"value\":[[1,10]]}]}";

      response = invoke(client, "/upsert_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[1,10]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[1,10]]}]}"
      );

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"subaddrs\":[{\"key\":0,\"value\":[[11,20]]}]}";
      response = invoke(client, "/upsert_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[11,20]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[1,20]]}]}"
      );
    }

    SECTION("feed check")
    {
      auto response = invoke_base(client, "/feed", "");
      EXPECT(response.second == 501);
      EXPECT(response.first == "");
    }

    SECTION("feed subscription")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height));
      namespace pull = lws_test::rpc::pull;

      const auto local = boost::asio::ip::make_address("127.0.0.1");
      boost::asio::io_context io;
      boost::asio::steady_timer timeout{io};

      timeout.expires_after(std::chrono::seconds{5});
      timeout.async_wait([&io] (auto) { io.stop(); });

      //
      // invalid version
      //
      std::shared_ptr<pull::connection> conn = 
        pull::make(io, boost::asio::ip::tcp::endpoint(local, 10000), {});

      bool ran = false;
      boost::system::error_code error{};
      std::string response;
      message = R"(login:{"account":{"address":")" + address + R"(","view_key":")" + viewkey + R"("}})";
      const auto handler =  [&] (auto ec, auto msg) { ran = true; error = ec; response = msg; };

      pull::async_handshake(conn, message, handler);

      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }
      
      EXPECT(!io.stopped());
      EXPECT(error == boost::beast::websocket::error::upgrade_declined);

      //
      // empty tx sync
      // 
      conn = pull::make(io, boost::asio::ip::tcp::endpoint(local, 10000), "lws.feed.v0.json");
      pull::async_handshake(conn, message, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      std::string expected =
        R"({"scanned_block_height":)" + scan_height + R"(,
          "start_height":)" + scan_height + R"(,
          "blockchain_height":)" + scan_height + "}";

      EXPECT(!io.stopped());
      EXPECT(error == boost::system::error_code{});
      EXPECT(get_prefix(response) == "tx_sync:");
      response.erase(0, std::strlen("tx_sync:"));
      verify_json(lest_env, "tx_sync", response, expected);

      const lws::db::transaction_link link{
        lws::db::block_id::txpool, crypto::rand<crypto::hash>()
      };
      const crypto::public_key tx_public = []() {
        crypto::secret_key secret;
        crypto::public_key out;
        crypto::generate_keys(out, secret);
        return out;
      }();
      const crypto::hash tx_prefix = crypto::rand<crypto::hash>();
      const crypto::public_key pub = crypto::rand<crypto::public_key>();
      const crypto::public_key pub2 = crypto::rand<crypto::public_key>();
      const rct::key ringct = crypto::rand<rct::key>();
      const auto extra = lws::db::extra(lws::db::extra::ringct_output);
      const auto payment_id_ = crypto::rand<lws::db::output::payment_id_>();

      rpc.push_update(
        get_full_account(),
        lws::mempool_receive{
          lws::db::output{
            link,
            lws::db::output::spend_meta_{
              lws::db::output_id::txpool(),
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
            lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(66)}
          }
        }
      );
      pull::async_next_message(conn, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      expected =
        R"({
          "hash":")" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + R"(",
          "prefix_hash":")" + epee::to_hex::string(epee::as_byte_span(tx_prefix)) + R"(",
          "fee":100,
          "unlock_time":4670,
          "payment_id":")" + epee::to_hex::string(epee::as_byte_span(payment_id_.long_)) + R"(",
          "mixin":16,
          "amount":40000,
          "public_key":")" + epee::to_hex::string(epee::as_byte_span(pub)) + R"(",
          "index":2,
          "tx_pub_key":")" + epee::to_hex::string(epee::as_byte_span(tx_public)) + R"(",
          "rct":")" + epee::to_hex::string(epee::as_byte_span(ringct)) + R"(",
          "recipient":{"maj_i":2,"min_i":66}
        })";

      EXPECT(!io.stopped());
      EXPECT(error == boost::system::error_code{});
      EXPECT(get_prefix(response) == "mempool:");
      response.erase(0, std::strlen("mempool:"));
      verify_json(lest_env, "mempool", response, expected);

      const lws::db::block_id new_height = lws::db::block_id(to_uint(last_block.id) + 1);
      const crypto::key_image image = crypto::rand<crypto::key_image>();
      {
        lws::account account = get_full_account();
        account.add_out(
          lws::db::output{
            {new_height, link.tx_hash},
            lws::db::output::spend_meta_{
              lws::db::output_id{0, 100},
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
            lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(66)}
          }
        );
        account.add_out(
          lws::db::output{
            {new_height, link.tx_hash},
            lws::db::output::spend_meta_{
              lws::db::output_id{0, 99},
              std::uint64_t(30000),
              std::uint32_t(16),
              std::uint32_t(1),
              tx_public
            },
            std::uint64_t(7000),
            std::uint64_t(4670),
            tx_prefix,
            pub2,
            ringct,
            {0, 0, 0, 0, 0, 0, 0},
            lws::db::pack(extra, sizeof(crypto::hash)),
            payment_id_,
            std::uint64_t(100),
            lws::db::address_index{lws::db::major_index(0), lws::db::minor_index(0)}
          }
        );

        account.add_spend(
          lws::db::spend{
            {new_height, link.tx_hash},
            image,
            lws::db::output_id{0, 90},
            std::uint64_t(7000),
            std::uint64_t(0),
            std::uint16_t(16),
            {0, 0, 0},
            std::uint8_t(0),
            {},
            {}
          }
        );
       
        const std::array<crypto::hash, 2> chain{{
          last_block.hash, crypto::rand<crypto::hash>()
        }};
        MONERO_UNWRAP(db.update(last_block.id, epee::to_span(chain), {std::addressof(account), 1}, nullptr));
        rpc.push_updates({std::addressof(account), 1}, new_height);
      }

      pull::async_next_message(conn, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      std::string new_height_str = std::to_string(to_uint(new_height));
      expected =
        R"({
          "scan_start":)" + scan_height + R"(,
          "scan_end":)" + new_height_str + R"(,
          "blockchain_height":)" + new_height_str + R"(,
          "transactions":[{
            "hash":")" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + R"(",
            "prefix_hash":")" + epee::to_hex::string(epee::as_byte_span(tx_prefix)) + R"(",
            "timestamp":7000,
            "fee":100,
            "unlock_time":4670,
            "height":)" + new_height_str + R"(,
            "payment_id":")" + epee::to_hex::string(epee::as_byte_span(payment_id_.long_)) + R"(",
            "mixin":16,
            "receives":[
              {
                "amount":40000,
                "public_key":")" + epee::to_hex::string(epee::as_byte_span(pub)) + R"(",
                "index":2,
                "id": {"legacy": {"amount": 0, "index": 100}},
                "tx_pub_key":")" + epee::to_hex::string(epee::as_byte_span(tx_public)) + R"(",
                "rct":")" + epee::to_hex::string(epee::as_byte_span(ringct)) + R"(",
                "recipient":{"maj_i":2,"min_i":66}
              },
              {
                "amount":30000,
                "public_key":")" + epee::to_hex::string(epee::as_byte_span(pub2)) + R"(",
                "index":1,
                "id": {"legacy": {"amount": 0, "index": 99}},
                "tx_pub_key":")" + epee::to_hex::string(epee::as_byte_span(tx_public)) + R"(",
                "rct":")" + epee::to_hex::string(epee::as_byte_span(ringct)) + R"("
              }
            ],
            "spends":[
              {
                "id": {"legacy": {"amount": 0, "index": 90}},
                "key_image":")" + epee::to_hex::string(epee::as_byte_span(image)) + R"("
              }
            ]
          }]
        })";

      EXPECT(!io.stopped());
      EXPECT(error == boost::system::error_code{});
      EXPECT(get_prefix(response) == "blocks:");
      response.erase(0, std::strlen("blocks:"));
      verify_json(lest_env, "blocks", response, expected);

      pull::async_close(conn, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      EXPECT(!io.stopped());
      EXPECT(error == boost::system::error_code{});
      EXPECT(response == "");

      // invalid view key
      message = R"(login:{"account":{"address":")" + address + R"(","view_key":")" + epee::to_hex::string(epee::as_byte_span(crypto::ec_scalar{})) + R"("}})";
      conn = pull::make(io, boost::asio::ip::tcp::endpoint(local, 10000), "lws.feed.v0.json");
      pull::async_handshake(conn, message, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      expected = R"({"msg":"Address/viewkey mismatch", "code":3})";

      EXPECT(!io.stopped());
      EXPECT(error == boost::system::error_code{});
      EXPECT(get_prefix(response) == "error:");
      response.erase(0, std::strlen("error:"));
      verify_json(lest_env, "view key error", response, expected);

      pull::async_next_message(conn, handler);

      ran = false;
      error = {};
      response = {};
      while (!ran && !io.stopped())
      {
        io.restart();
        io.run_one();
      }

      EXPECT(!io.stopped());
      EXPECT(error == boost::beast::websocket::error::closed); 
    }
  }
}

