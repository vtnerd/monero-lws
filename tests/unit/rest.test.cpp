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

#include <optional>
#include "db/data.h"
#include "db/storage.test.h"
#include "db/string.h"
#include "error.h"
#include "hex.h" // monero/epee/contrib/include
#include "net/http_client.h"
#include "rest_server.h"
#include "scanner.test.h"
#include "util/account.h"

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

  struct user_account
  {
    lws::db::account_address account;
    crypto::secret_key view;

    user_account(const lws::db::account_address& account, const crypto::secret_key& view)
      : account(account), view(view)
    {}
  };

  struct carrot_account
  {
    lws::carrot_account account;
    crypto::secret_key generate_address;

    carrot_account()
      : account{}, generate_address{}
    {}
  };

  std::string invoke(enet::http::http_simple_client& client, const boost::string_ref uri, const boost::string_ref body)
  {
    const enet::http::http_response_info* info = nullptr;
    if (!client.invoke(uri, "POST", body, std::chrono::milliseconds{500}, std::addressof(info), {}))
      throw std::runtime_error{"HTTP invoke failed"};
    if (info->m_response_code != 200)
      throw std::runtime_error{"HTTP invoke not 200, instead " + std::to_string(info->m_response_code)};
    return std::string{info->m_body}; 
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

  void check_address_map(lest::env& lest_env, lws::db::storage& db, const user_account& user, const std::pair<std::uint32_t, std::uint32_t> subaddress)
  {
    SETUP("check_address_map")
    {
      auto reader = MONERO_UNWRAP(db.start_read());
      const lws::db::address_index index{
        lws::db::major_index(subaddress.first),
        lws::db::minor_index(subaddress.second)
      };

      lws::db::cursor::subaddress_indexes cur = nullptr;
      auto result = reader.find_subaddress(lws::db::account_id(1), index.get_spend_public(user.account, user.view), cur);
      EXPECT(result.has_value());
      EXPECT(result == index);
    }
  }

  void check_address_map(lest::env& lest_env, lws::db::storage& db, const carrot_account& user, const std::pair<std::uint32_t, std::uint32_t> subaddress)
  {
    SETUP("check_address_map")
    {
      auto reader = MONERO_UNWRAP(db.start_read());
      const lws::db::address_index index{
        lws::db::major_index(subaddress.first),
        lws::db::minor_index(subaddress.second)
      };

      lws::db::cursor::subaddress_indexes cur = nullptr;
      auto result = reader.find_subaddress(lws::db::account_id(1), index.get_spend_public(user.account, user.generate_address), cur);
      EXPECT(result.has_value());
      EXPECT(result == index);
    }
  }
}

LWS_CASE("rest_server")
{
  lws::db::account_address account{};
  carrot_account account_incoming{};
  crypto::secret_key view{};
  crypto::generate_keys(account.spend_public, view);
  crypto::generate_keys(account.view_public, view);
  const std::string address = lws::db::address_string(account);
  const std::string viewkey = epee::to_hex::string(epee::as_byte_span(unwrap(unwrap(view))));
  {
    lws::db::account temp{};
    crypto::public_key temp2{};
    temp.address = account;
    std::memcpy(std::addressof(temp.key), std::addressof(unwrap(unwrap(view))), sizeof(temp.key));
    account_incoming.account = lws::carrot_account{temp};
    crypto::generate_keys(temp2, account_incoming.generate_address);
  }
  const std::string generatekey = epee::to_hex::string(epee::as_byte_span(unwrap(unwrap(account_incoming.generate_address))));
  const user_account account_legacy{account, view};

  SETUP("Database and login")
  {
    std::optional<lws::rest_server> server;
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    auto context =
      lws::rpc::context::make(lws_test::rpc_rendevous, {}, {}, {}, std::chrono::minutes{0}, false);
    const auto rpc = MONERO_UNWRAP(context.connect());
    {
      const lws::rest_server::configuration config{
        {}, {}, 1, 20, {}, false, true, true
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
    const auto get_account = [&db, &account] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account)).second;
    };

    enet::http::http_simple_client client{};
    client.set_server("127.0.0.1", "10000", boost::none);
    EXPECT(client.connect(std::chrono::milliseconds{500})); 

    std::string message =
      "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"create_account\":true,\"generated_locally\":true}";
    std::string response = invoke(client, "/login", message);
    EXPECT(response == "{\"new_address\":true,\"generated_locally\":true}");

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

      lws::account real_account{account, {}, {}, {}};
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
          "\"mixin\":16}"
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

      lws::account real_account{account, {}, {}, {}};
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

    SECTION("One Carrot Receive, One Carrot Spend")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\"}";

      const lws::db::transaction_link link{
        lws::db::block_id(5000), crypto::rand<crypto::hash>()
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
        lws::db::extra(lws::db::extra::ringct_output);
      const auto payment_id_ = crypto::rand<lws::db::output::payment_id_>();
      const crypto::hash payment_id = crypto::rand<crypto::hash>();
      const crypto::key_image image = crypto::rand<crypto::key_image>();
      const crypto::key_image first_ki = crypto::rand<crypto::key_image>();
      const auto anchor = crypto::rand<carrot::encrypted_janus_anchor_t>();

      lws::account real_account{account, {}, {}, {}};
      real_account.add_out(
        lws::db::output{
          link,
          lws::db::output::spend_meta_{
            lws::db::output_id{350, 0},
            std::uint64_t(70000),
            lws::db::carrot_internal,
            std::uint32_t(1),
            tx_public
          },
          std::uint64_t(9000),
          std::uint64_t(5670),
          tx_prefix,
          pub,
          ringct,
          {0, 0, 0, 0, 0, 0, 0},
          lws::db::pack(extra, sizeof(crypto::hash8)),
          payment_id_,
          std::uint64_t(500),
          lws::db::address_index{lws::db::major_index(0), lws::db::minor_index(0)},
          first_ki,
          anchor
        }
      );
      real_account.add_spend(
        lws::db::spend{
          link,
          image,
          lws::db::output_id::unknown_spend(),
          std::uint64_t(86),
          std::uint64_t(9500),
          lws::db::carrot_external,
          {0, 0, 0},
          0,
          payment_id,
          lws::db::address_index{lws::db::major_index(0), lws::db::minor_index(0)}
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
        "\"total_received\":\"70000\","
        "\"total_sent\":\"0\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"spent_outputs\":[{"
          "\"amount\":\"0\","
          "\"key_image\":\"" + epee::to_hex::string(epee::as_byte_span(image)) + "\","
          "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(crypto::public_key{})) + "\","
          "\"out_index\":0,"
          "\"mixin\":4294967295,"
          "\"sender\":{\"maj_i\":0,\"min_i\":0}"
        "}]}"
      );

      response = invoke(client, "/get_address_txs", message);
      EXPECT(response ==
        "{\"total_received\":\"70000\","
        "\"scanned_height\":" + scan_height + "," +
        "\"scanned_block_height\":" + scan_height + ","
        "\"start_height\":" + start_height + ","
        "\"transaction_height\":" + scan_height + ","
        "\"blockchain_height\":" + scan_height + ","
        "\"transactions\":["
          "{\"id\":0,"
          "\"hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"timestamp\":\"1970-01-01T02:30:00Z\","
          "\"total_received\":\"70000\","
          "\"total_sent\":\"0\","
          "\"fee\":\"500\","
          "\"unlock_time\":5670,"
          "\"height\":5000,"
          "\"payment_id\":\"" + epee::to_hex::string(epee::as_byte_span(payment_id_.short_)) + "\","
          "\"coinbase\":false,"
          "\"mempool\":false,"
          "\"mixin\":4294967295,"
          "\"spent_outputs\":[{"
            "\"amount\":\"0\","
            "\"key_image\":\"" + epee::to_hex::string(epee::as_byte_span(image)) + "\","
            "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(crypto::public_key{})) + "\","
            "\"out_index\":0,"
            "\"mixin\":4294967295,"
            "\"sender\":{\"maj_i\":0,\"min_i\":0}"
          "}]}"
        "]}"
      );

      std::vector<epee::byte_slice> messages;
      messages.emplace_back(get_fee_response());
      boost::thread server_thread(&lws_test::rpc_thread, context.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};

      const auto ringct_expanded = get_rct_bytes(view, tx_public, ringct, 70000, 1, false);
      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"amount\":\"0\"}";
      response = invoke(client, "/get_unspent_outs", message);
      EXPECT(response ==
        "{\"per_byte_fee\":39,"
        "\"fee_mask\":1000,"
        "\"amount\":\"70000\","
        "\"outputs\":["
          "{\"amount\":\"70000\","
          "\"public_key\":\"" + epee::to_hex::string(epee::as_byte_span(pub)) + "\","
          "\"index\":1,"
          "\"global_index\":0,"
          "\"tx_id\":0,"
          "\"tx_hash\":\"" + epee::to_hex::string(epee::as_byte_span(link.tx_hash)) + "\","
          "\"tx_prefix_hash\":\"" + epee::to_hex::string(epee::as_byte_span(tx_prefix)) + "\","
          "\"tx_pub_key\":\"" + epee::to_hex::string(epee::as_byte_span(tx_public)) + "\","
          "\"timestamp\":\"1970-01-01T02:30:00Z\","
          "\"height\":5000,"
          "\"rct\":\"" + epee::to_hex::string(epee::as_byte_span(ringct_expanded)) + "\","
          "\"recipient\":{\"maj_i\":0,\"min_i\":0},"
          "\"first_key_image\":\"" + epee::to_hex::string(epee::as_byte_span(first_ki)) + "\","
          "\"janus_anchor\":\"" + epee::to_hex::string(epee::as_byte_span(anchor)) + "\"}"
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

      check_address_map(lest_env, db, account_legacy, {0, 4});

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

      check_address_map(lest_env, db, account_legacy, {3, 9});
    }

    SECTION("provision_subaddrs carrot incoming-only")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message =
        "{\"address\":\"" + address + "\","
        "\"view_key\":\"" + viewkey + "\","
        "\"generate_address_key\":\"" + generatekey + "\","
        "\"maj_i\":0,\"min_i\":0,\"n_maj\":2,\"n_min\":5}";

      response = invoke(client, "/provision_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[0,4]]},"
          "{\"key\":1,\"value\":[[0,4]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[0,4]]},"
          "{\"key\":1,\"value\":[[0,4]]}]}"
      );

      check_address_map(lest_env, db, account_incoming, {0, 4});
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

      check_address_map(lest_env, db, account_legacy, {0, 10});

      message = "{\"address\":\"" + address + "\",\"view_key\":\"" + viewkey + "\",\"subaddrs\":[{\"key\":0,\"value\":[[11,20]]}]}";
      response = invoke(client, "/upsert_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[11,20]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[1,20]]}]}"
      );

      check_address_map(lest_env, db, account_legacy, {0, 20});
    }

    SECTION("upsert_subaddrs carrot incoming-only")
    {
      const std::string scan_height = std::to_string(std::uint64_t(account.scan_height) + 5);
      const std::string start_height = std::to_string(std::uint64_t(account.start_height));
      message =
        "{\"address\":\"" + address + "\","
        "\"view_key\":\"" + viewkey + "\","
        "\"generate_address_key\":\"" + generatekey + "\","
        "\"subaddrs\":[{\"key\":0,\"value\":[[1,10]]}]}";

      response = invoke(client, "/upsert_subaddrs", message);
      EXPECT(response ==
        "{\"new_subaddrs\":["
          "{\"key\":0,\"value\":[[1,10]]}"
        "],\"all_subaddrs\":["
          "{\"key\":0,\"value\":[[1,10]]}]}"
      );

      check_address_map(lest_env, db, account_incoming, {0, 10});
    }
  }
}

