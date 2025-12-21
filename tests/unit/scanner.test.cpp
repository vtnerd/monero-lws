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

#include "scanner.test.h"
#include "framework.test.h"

#include <boost/thread.hpp>

#include "cryptonote_basic/account.h" // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_config.h"        // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h"      // monero/src
#include "db/chain.test.h"
#include "db/print.test.h"
#include "db/storage.test.h"
#include "device/device_default.hpp" // monero/src
#include "hardforks/hardforks.h"     // monero/src
#include "net/zmq.h"                 // monero/src
#include "rpc/client.h"
#include "rpc/daemon_messages.h"     // monero/src
#include "scanner.h"
#include "wire/error.h"
#include "wire/json/write.h"

namespace
{
  constexpr const std::chrono::seconds message_timeout{3};

  template<typename T>
  struct json_rpc_response
  {
    T result;
    std::uint32_t id = 0;
  };

  template<typename T>
  void write_bytes(wire::json_writer& dest, const json_rpc_response<T>& self)
  {
    wire::object(dest, WIRE_FIELD(id), WIRE_FIELD(result));
  }

  template<typename T>
  epee::byte_slice to_json_rpc(T message)
  {
    epee::byte_slice out{};
    const std::error_code err =
      wire::json::to_bytes(out, json_rpc_response<T>{std::move(message)});
    if (err)
      MONERO_THROW(err, "Failed to serialize json_rpc_response");
    return out;
  }

  template<typename T>
  epee::byte_slice daemon_response(const T& message)
  {
    rapidjson::Value id;
    id.SetInt(0);
    return cryptonote::rpc::FullMessage::getResponse(message, id);
  }

  struct join
  {
      boost::thread& thread;
      ~join() { thread.join(); }
  };

  struct transaction
  {
    cryptonote::transaction tx;
    std::vector<crypto::secret_key> additional_keys;
    std::vector<crypto::public_key> pub_keys;
    std::vector<crypto::public_key> spend_publics;
  };

  transaction make_miner_tx(lest::env& lest_env, lws::db::block_id height, const lws::db::account_address& miner_address, bool use_view_tags)
  {
    static constexpr std::uint64_t fee = 0;

    transaction tx{};
    tx.pub_keys.emplace_back();
    tx.spend_publics.emplace_back();

    crypto::secret_key key;
    crypto::generate_keys(tx.pub_keys.back(), key);
    EXPECT(add_tx_pub_key_to_extra(tx.tx, tx.pub_keys.back()));

    cryptonote::txin_gen in;
    in.height = std::uint64_t(height);
    tx.tx.vin.push_back(in);

    // This will work, until size of constructed block is less then CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE
    uint64_t block_reward;
    EXPECT(cryptonote::get_block_reward(0, 0, 1000000, block_reward, num_testnet_hard_forks));
    block_reward += fee;

    crypto::key_derivation derivation;
    EXPECT(crypto::generate_key_derivation(miner_address.view_public, key, derivation));
    EXPECT(crypto::derive_public_key(derivation, 0, miner_address.spend_public, tx.spend_publics.back()));
 
    crypto::view_tag view_tag;
    if (use_view_tags)
      crypto::derive_view_tag(derivation, 0, view_tag);

    cryptonote::tx_out out;
    cryptonote::set_tx_out(block_reward, tx.spend_publics.back(), use_view_tags, view_tag, out);

    tx.tx.vout.push_back(out);
    tx.tx.version = 2;
    tx.tx.unlock_time = std::uint64_t(height) + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;

    return tx;
  }

  struct get_spend_public
  {
    std::vector<crypto::public_key>& pub_keys;

    template<typename T>
    void operator()(const T&) const noexcept
    {}
   
    void operator()(const cryptonote::txout_to_key& val) const
    { pub_keys.push_back(val.key); }

    void operator()(const cryptonote::txout_to_tagged_key& val) const
    { pub_keys.push_back(val.key); }
  };

  transaction make_tx(lest::env& lest_env, const cryptonote::account_keys& keys, std::vector<cryptonote::tx_destination_entry> destinations, const std::uint32_t ring_base, const bool use_view_tag)
  {
    static constexpr std::uint64_t input_amount = 20000;
    static constexpr std::uint64_t output_amount = 8000;

    EXPECT(15 < std::numeric_limits<std::uint32_t>::max() - ring_base);

    crypto::secret_key unused_key{};
    crypto::secret_key og_tx_key{};
    crypto::public_key og_tx_public{};
    crypto::generate_keys(og_tx_public, og_tx_key);

    crypto::key_derivation derivation{};
    crypto::public_key spend_public{};
    EXPECT(crypto::generate_key_derivation(keys.m_account_address.m_view_public_key, og_tx_key, derivation));
    EXPECT(crypto::derive_public_key(derivation, 0, keys.m_account_address.m_spend_public_key, spend_public));

    std::uint32_t index = -1;
    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    for (const auto& destination : destinations)
    {
      ++index;
      subaddresses[destination.addr.m_spend_public_key] = {0, index};
    }

    if (2 < destinations.size())
      destinations.erase(destinations.begin() + 1, destinations.end() - 1);

    std::vector<cryptonote::tx_source_entry> sources;
    sources.emplace_back();
    sources.back().amount = input_amount;
    sources.back().rct = true;
    sources.back().real_output = 15;
    sources.back().real_output_in_tx_index = 0;
    sources.back().real_out_tx_key = og_tx_public;
    for (std::uint32_t i = ring_base; i < 15 + ring_base; ++i)
    {
      crypto::public_key next{};
      crypto::generate_keys(next, unused_key);
      sources.back().push_output(i, next, 10000);
    }
    sources.back().outputs.emplace_back();
    sources.back().outputs.back().first = 15 + ring_base;
    sources.back().outputs.back().second.dest = rct::pk2rct(spend_public);
 
    transaction out{};
    EXPECT(
      cryptonote::construct_tx_and_get_tx_key(
        keys, subaddresses, sources, destinations, keys.m_account_address, {}, out.tx, /* 0, */ unused_key,
        out.additional_keys, true, {rct::RangeProofType::RangeProofPaddedBulletproof, 2}, use_view_tag
      )
    );

    for (const auto& vout : out.tx.vout)
      boost::apply_visitor(get_spend_public{out.spend_publics}, vout.target);

    if (out.additional_keys.empty())
    {
      std::vector<cryptonote::tx_extra_field> extra;
      EXPECT(cryptonote::parse_tx_extra(out.tx.extra, extra));
 
      cryptonote::tx_extra_pub_key key;
      EXPECT(cryptonote::find_tx_extra_field_by_type(extra, key));

      out.pub_keys.emplace_back();
      out.pub_keys.back() = key.pub_key;
    }
    else
    {
      for (const auto& this_key : out.additional_keys)
      {
        out.pub_keys.emplace_back();
        EXPECT(crypto::secret_key_to_public_key(this_key, out.pub_keys.back()));
      }
    }
    return out;
  }

  void scanner_thread(lws::scanner& scanner, void* ctx, const std::vector<epee::byte_slice>& reply)
  {
    struct stop_
    {
      lws::scanner& scanner;
      ~stop_() { scanner.shutdown(); }; 
    } stop{scanner};

    lws_test::rpc_thread(ctx, reply);
  }
} // anonymous

namespace lws_test
{
  void rpc_thread(void* ctx, const std::vector<epee::byte_slice>& reply)
  {
    try
    {
      net::zmq::socket server{};
      server.reset(zmq_socket(ctx, ZMQ_REP));
      if (!server || zmq_bind(server.get(), lws_test::rpc_rendevous))
      {
        std::cout << "Failed to create ZMQ server" << std::endl;
        return;
      }

      for (const epee::byte_slice& message : reply)
      {
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
          const auto request = net::zmq::receive(server.get(), ZMQ_DONTWAIT);
          if (request)
            break;

          if (request != net::zmq::make_error_code(EAGAIN))
          {
            std::cout << "Failed to retrieve message in fake ZMQ server: " << request.error().message() << std::endl;;
            return;
          }

          if (message_timeout <= std::chrono::steady_clock::now() - start)
          {
            std::cout << "Timeout in dummy RPC server" << std::endl;
            return;
          }
          boost::this_thread::sleep_for(boost::chrono::milliseconds{10});
        } // until error or received message

        const auto sent = net::zmq::send(message.clone(), server.get());
        if (!sent)
        {
          std::cout << "Failed to send dummy RPC message: " << sent.error().message() << std::endl;
          return;
        }
      } // foreach message
    }
    catch (const std::exception& e)
    {
      std::cout << "Unexpected exception in dummy RPC server: " << e.what() << std::endl;
    }
  }
}

LWS_CASE("lws::scanner::sync and lws::scanner::run")
{
  cryptonote::account_keys keys{};
  crypto::generate_keys(keys.m_account_address.m_spend_public_key, keys.m_spend_secret_key);
  crypto::generate_keys(keys.m_account_address.m_view_public_key, keys.m_view_secret_key);

  const lws::db::account_address account{
    keys.m_account_address.m_view_public_key,
    keys.m_account_address.m_spend_public_key
  };

  cryptonote::account_keys keys_subaddr1{};
  cryptonote::account_keys keys_subaddr2{};
  {
    hw::core::device_default hw{};
    keys_subaddr1.m_account_address = hw.get_subaddress(keys, cryptonote::subaddress_index{0, 1});
    keys_subaddr2.m_account_address = hw.get_subaddress(keys, cryptonote::subaddress_index{0, 2});

    const auto sub1_secret = hw.get_subaddress_secret_key(keys.m_view_secret_key, cryptonote::subaddress_index{0, 1});
    const auto sub2_secret = hw.get_subaddress_secret_key(keys.m_view_secret_key, cryptonote::subaddress_index{0, 2});

    sc_add(to_bytes(keys_subaddr1.m_spend_secret_key), to_bytes(sub1_secret), to_bytes(keys.m_spend_secret_key));
    sc_add(to_bytes(keys_subaddr1.m_view_secret_key), to_bytes(keys_subaddr1.m_spend_secret_key), to_bytes(keys.m_view_secret_key));

    sc_add(to_bytes(keys_subaddr2.m_spend_secret_key), to_bytes(sub2_secret), to_bytes(keys.m_spend_secret_key));
    sc_add(to_bytes(keys_subaddr2.m_view_secret_key), to_bytes(keys_subaddr2.m_spend_secret_key), to_bytes(keys.m_view_secret_key));
  } 

  cryptonote::account_keys keys2{};
  crypto::generate_keys(keys2.m_account_address.m_spend_public_key, keys2.m_spend_secret_key);
  crypto::generate_keys(keys2.m_account_address.m_view_public_key, keys2.m_view_secret_key);

  const lws::db::account_address account2{
    keys2.m_account_address.m_view_public_key,
    keys2.m_account_address.m_spend_public_key
  };

  SETUP("lws::rpc::context, ZMQ_REP Server, and lws::db::storage")
  {
    auto rpc = 
      lws::rpc::context::make(lws_test::rpc_rendevous, {}, {}, {}, std::chrono::minutes{0}, false);


    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());

    const auto get_account = [&db, &account] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account)).second;
    };

    SECTION("lws::scanner::sync Invalid Response")
    {
      const crypto::hash hashes[1] = {
        last_block.hash
      };

      std::vector<epee::byte_slice> messages{};
      messages.push_back(to_json_rpc(1));

      lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};

      boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};
      EXPECT(!scanner.sync(MONERO_UNWRAP(rpc.connect())));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, hashes);
    }

    SECTION("lws::scanner::sync Update")
    {
      std::vector<epee::byte_slice> messages{};
      std::vector<crypto::hash> hashes{
        last_block.hash,
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>()
      };

      cryptonote::rpc::GetHashesFast::Response message{};

      message.start_height = std::uint64_t(last_block.id);
      message.hashes = hashes;
      message.current_height = message.start_height + hashes.size() - 1;
      messages.push_back(daemon_response(message));

      message.start_height = message.current_height;
      message.hashes.front() = message.hashes.back();
      message.hashes.resize(1);
      messages.push_back(daemon_response(message));

      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, {hashes.data(), 1});
      {
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
        lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
      }

      SECTION("Fork Chain")
      {
        messages.clear();
        hashes[2] = crypto::rand<crypto::hash>();
        hashes[3] = crypto::rand<crypto::hash>();
        hashes[4] = crypto::rand<crypto::hash>();
        hashes[5] = crypto::rand<crypto::hash>();

        message.start_height = std::uint64_t(last_block.id);
        message.hashes = hashes;
        messages.push_back(daemon_response(message));

        message.start_height = message.current_height;
        message.hashes.front() = message.hashes.back();
        message.hashes.resize(1);
        messages.push_back(daemon_response(message));

        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
        lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
      }
    }

    SECTION("lws::scanner::run (with upsert)")
    {
      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
        const auto result =
          db.upsert_subaddresses(lws::db::account_id(1), account, keys.m_view_secret_key, indexes, 2);
        EXPECT(result);
        EXPECT(result->size() == 1);
        EXPECT(result->at(0).first == lws::db::major_index::primary);
        EXPECT(result->at(0).second.get_container().size() == 1);
        EXPECT(result->at(0).second.get_container().at(0).size() == 2);
        EXPECT(result->at(0).second.get_container().at(0).at(0) == lws::db::minor_index(1));
        EXPECT(result->at(0).second.get_container().at(0).at(1) == lws::db::minor_index(2));
      }

      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = keys.m_account_address;

      std::vector<epee::byte_slice> messages{};
      transaction tx = make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      transaction tx2 = make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);

      transaction tx3 = make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx4 = make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      //destinations.emplace_back();
      //destinations.back().amount = 1000;
      //destinations.back().addr = keys_subaddr2.m_account_address;
      //destinations.back().is_subaddress = true;

      //transaction tx5 = make_tx(lest_env, keys, destinations, 100, true);
      //EXPECT(tx5.pub_keys.size() == 3);
      //EXPECT(tx5.spend_publics.size() == 3);

      cryptonote::rpc::GetBlocksFast::Response bmessage{};
      bmessage.start_height = std::uint64_t(last_block.id) + 1;
      bmessage.current_height = bmessage.start_height + 1;
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = tx.tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx2.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx3.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx4.tx));
      bmessage.blocks.back().transactions.push_back(tx2.tx);
      bmessage.blocks.back().transactions.push_back(tx3.tx);
      bmessage.blocks.back().transactions.push_back(tx4.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(100);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(101);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(102);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(200);
      bmessage.output_indices.back().back().push_back(201);
      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());

      std::vector<crypto::hash> hashes{
        last_block.hash,
        cryptonote::get_block_hash(bmessage.blocks.back().block),
      };
      {
        cryptonote::rpc::GetHashesFast::Response hmessage{};

        hmessage.start_height = std::uint64_t(last_block.id);
        hmessage.hashes = hashes;
        hmessage.current_height = hmessage.start_height + hashes.size() - 1;
        messages.push_back(daemon_response(hmessage));

        hmessage.start_height = hmessage.current_height;
        hmessage.hashes.front() = hmessage.hashes.back();
        hmessage.hashes.resize(1);
        messages.push_back(daemon_response(hmessage));

        {
          lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
          boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
          const join on_scope_exit{server_thread};
          EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
          lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
        }
      }

      EXPECT(db.add_account(account, keys.m_view_secret_key));
      EXPECT(db.add_account(account2, keys2.m_view_secret_key));

      messages.clear();
      messages.push_back(daemon_response(bmessage));
      bmessage.start_height = bmessage.current_height;
      bmessage.blocks.resize(1);
      bmessage.output_indices.resize(1);
      messages.push_back(daemon_response(bmessage));
      {
        static constexpr const lws::scanner_options opts{0, 0, lws::MINIMUM_BLOCK_DEPTH, 1, false, false, false, false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        scanner.run(std::move(rpc), 1, {}, {}, opts);
      }

      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.back().block));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));

      const lws::db::block_id new_last_block_id = lws::db::block_id(std::uint64_t(last_block.id) + 2);
      EXPECT(get_account().scan_height == new_last_block_id);
      {
        const std::map<std::pair<lws::db::output_id, std::uint32_t>, lws::db::output> expected{
          {
            {lws::db::output_id{0, 100}, 35184372088830}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 100}, 35184372088830, 0, 0, tx.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx.tx),
              tx.spend_publics.at(0),
              rct::commit(35184372088830, rct::identity()),
              {},
              lws::db::pack(lws::db::extra(lws::db::extra::coinbase_output | lws::db::extra::ringct_output), 0),
              {},
              0, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 101}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx2.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 101}, 8000, 15, 0, tx2.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx2.tx),
              tx2.spend_publics.at(0),
              tx2.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
	        {
            {lws::db::output_id{0, 102}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx3.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 102}, 8000, 15, 0, tx3.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx3.tx),
              tx3.spend_publics.at(0),
              tx3.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 200}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 8000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 201}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 8000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 200}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 2000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 201}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 2000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          }
        };

        auto reader = MONERO_UNWRAP(db.start_read());
        auto outputs = MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(1)));
        EXPECT(outputs.count() == 5);
        auto output_it = outputs.make_iterator();
        for (auto output_it = outputs.make_iterator(); !output_it.is_end(); ++output_it)
        {
          auto real_output = *output_it;
          const auto expected_output =
            expected.find(std::make_pair(real_output.spend_meta.id, real_output.spend_meta.amount));
          EXPECT(expected_output != expected.end());

          EXPECT(real_output.link.height == expected_output->second.link.height);
          EXPECT(real_output.link.tx_hash == expected_output->second.link.tx_hash);
          EXPECT(real_output.spend_meta.id == expected_output->second.spend_meta.id);
          EXPECT(real_output.spend_meta.amount == expected_output->second.spend_meta.amount);
          EXPECT(real_output.spend_meta.mixin_count == expected_output->second.spend_meta.mixin_count);
          EXPECT(real_output.spend_meta.index == expected_output->second.spend_meta.index);
          EXPECT(real_output.tx_prefix_hash == expected_output->second.tx_prefix_hash);
          EXPECT(real_output.spend_meta.tx_public == expected_output->second.spend_meta.tx_public);
          EXPECT(real_output.pub == expected_output->second.pub);
          EXPECT(rct::commit(real_output.spend_meta.amount, real_output.ringct_mask) == expected_output->second.ringct_mask);
          EXPECT(real_output.extra == expected_output->second.extra);
          if (unpack(expected_output->second.extra).second == 8)
            EXPECT(real_output.payment_id.short_ == expected_output->second.payment_id.short_);
          EXPECT(real_output.fee == expected_output->second.fee);
          EXPECT(real_output.recipient == expected_output->second.recipient);
        }

        auto spends = MONERO_UNWRAP(reader.get_spends(lws::db::account_id(1)));
        EXPECT(spends.count() == 2);
        auto spend_it = spends.make_iterator();
        EXPECT(!spend_it.is_end());

        auto real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        lws::db::output_id expected_out{0, 100};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        ++spend_it;
        EXPECT(!spend_it.is_end());

        real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        expected_out = lws::db::output_id{0, 101};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        EXPECT(MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(2))).count() == 0);
        EXPECT(MONERO_UNWRAP(reader.get_spends(lws::db::account_id(2))).count() == 0);
      }
    } //SECTION (lws::scanner::run (with upsert))

    SECTION("lws::scanner::run (with lookahead)")
    {
      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = keys.m_account_address;

      std::vector<epee::byte_slice> messages{};
      transaction tx = make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      transaction tx2 = make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);

      transaction tx3 = make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx4 = make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      destinations.emplace_back();
      destinations.back().amount = 1000;
      destinations.back().addr = keys_subaddr2.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx5 = make_tx(lest_env, keys, destinations, 146, true);
      EXPECT(tx5.pub_keys.size() == 1);
      EXPECT(tx5.spend_publics.size() == 2);

      cryptonote::rpc::GetBlocksFast::Response bmessage{};
      bmessage.start_height = std::uint64_t(last_block.id) + 1;
      bmessage.current_height = bmessage.start_height + 1;
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = tx.tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx2.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx3.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx4.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx5.tx));
      bmessage.blocks.back().transactions.push_back(tx2.tx);
      bmessage.blocks.back().transactions.push_back(tx3.tx);
      bmessage.blocks.back().transactions.push_back(tx4.tx);
      bmessage.blocks.back().transactions.push_back(tx5.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(100);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(101);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(102);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(200);
      bmessage.output_indices.back().back().push_back(201);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(300);
      bmessage.output_indices.back().back().push_back(301);
      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());

      std::vector<crypto::hash> hashes{
        last_block.hash,
        cryptonote::get_block_hash(bmessage.blocks.back().block),
      };
      {
        cryptonote::rpc::GetHashesFast::Response hmessage{};

        hmessage.start_height = std::uint64_t(last_block.id);
        hmessage.hashes = hashes;
        hmessage.current_height = hmessage.start_height + hashes.size() - 1;
        messages.push_back(daemon_response(hmessage));

        hmessage.start_height = hmessage.current_height;
        hmessage.hashes.front() = hmessage.hashes.back();
        hmessage.hashes.resize(1);
        messages.push_back(daemon_response(hmessage));

        {
          lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
          boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
          const join on_scope_exit{server_thread};
          EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
          lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
        }
      }

      EXPECT(db.add_account(account, keys.m_view_secret_key));
      EXPECT(db.add_account(account2, keys2.m_view_secret_key));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{};
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      const lws::db::block_id user_height =
        MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(lws::db::account_status::active, lws::db::account_id(1))).scan_height;

      EXPECT(db.import_request(account, user_height, {lws::db::major_index(1), lws::db::minor_index(2)}));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account), 1}, 2));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{
          {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}}
        };
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      messages.clear();
      messages.push_back(daemon_response(bmessage));
      bmessage.start_height = bmessage.current_height;
      bmessage.blocks.resize(1);
      bmessage.output_indices.resize(1);
      messages.push_back(daemon_response(bmessage));
      {
        static constexpr const lws::scanner_options opts{0, 0, lws::MINIMUM_BLOCK_DEPTH, 10, false, false, false, false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        scanner.run(std::move(rpc), 1, {}, {}, opts);
      }

      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.back().block));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));

      const lws::db::block_id new_last_block_id = lws::db::block_id(std::uint64_t(last_block.id) + 2);
      EXPECT(get_account().scan_height == new_last_block_id);
      {
        const std::map<std::pair<lws::db::output_id, std::uint32_t>, lws::db::output> expected{
          {
            {lws::db::output_id{0, 100}, 35184372088830}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 100}, 35184372088830, 0, 0, tx.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx.tx),
              tx.spend_publics.at(0),
              rct::commit(35184372088830, rct::identity()),
              {},
              lws::db::pack(lws::db::extra(lws::db::extra::coinbase_output | lws::db::extra::ringct_output), 0),
              {},
              0, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 101}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx2.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 101}, 8000, 15, 0, tx2.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx2.tx),
              tx2.spend_publics.at(0),
              tx2.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
	        {
            {lws::db::output_id{0, 102}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx3.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 102}, 8000, 15, 0, tx3.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx3.tx),
              tx3.spend_publics.at(0),
              tx3.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 200}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 8000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 201}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 8000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 200}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 2000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 201}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 2000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 300}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 300}, 8000, 15, 0, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(0),
              tx5.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 301}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 301}, 8000, 15, 1, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(1),
              tx5.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 300}, 1000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 300}, 1000, 15, 0, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(0),
              tx5.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(2)}
            }
          },
          {
            {lws::db::output_id{0, 301}, 1000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 301}, 1000, 15, 1, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(1),
              tx5.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(2)}
            }
          }
        };

        auto reader = MONERO_UNWRAP(db.start_read());
        auto outputs = MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(1)));
        EXPECT(outputs.count() == 6);
        auto output_it = outputs.make_iterator();
        for (auto output_it = outputs.make_iterator(); !output_it.is_end(); ++output_it)
        {
          auto real_output = *output_it;
          const auto expected_output =
            expected.find(std::make_pair(real_output.spend_meta.id, real_output.spend_meta.amount));
          EXPECT(expected_output != expected.end());

          EXPECT(real_output.link.height == expected_output->second.link.height);
          EXPECT(real_output.link.tx_hash == expected_output->second.link.tx_hash);
          EXPECT(real_output.spend_meta.id == expected_output->second.spend_meta.id);
          EXPECT(real_output.spend_meta.amount == expected_output->second.spend_meta.amount);
          EXPECT(real_output.spend_meta.mixin_count == expected_output->second.spend_meta.mixin_count);
          EXPECT(real_output.spend_meta.index == expected_output->second.spend_meta.index);
          EXPECT(real_output.tx_prefix_hash == expected_output->second.tx_prefix_hash);
          EXPECT(real_output.spend_meta.tx_public == expected_output->second.spend_meta.tx_public);
          EXPECT(real_output.pub == expected_output->second.pub);
          EXPECT(rct::commit(real_output.spend_meta.amount, real_output.ringct_mask) == expected_output->second.ringct_mask);
          EXPECT(real_output.extra == expected_output->second.extra);
          if (unpack(expected_output->second.extra).second == 8)
            EXPECT(real_output.payment_id.short_ == expected_output->second.payment_id.short_);
          EXPECT(real_output.fee == expected_output->second.fee);
          EXPECT(real_output.recipient == expected_output->second.recipient);
        }

        auto spends = MONERO_UNWRAP(reader.get_spends(lws::db::account_id(1)));
        EXPECT(spends.count() == 2);
        auto spend_it = spends.make_iterator();
        EXPECT(!spend_it.is_end());

        auto real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        lws::db::output_id expected_out{0, 100};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        ++spend_it;
        EXPECT(!spend_it.is_end());

        real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        expected_out = lws::db::output_id{0, 101};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        EXPECT(MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(2))).count() == 0);
        EXPECT(MONERO_UNWRAP(reader.get_spends(lws::db::account_id(2))).count() == 0);

        {
          const std::vector<lws::db::subaddress_dict> expected_range{
            {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(2)}}}}
          };
          EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
        }
      }
    } //SECTION (lws::scanner::run (lookahead))
  } // SETUP
} // LWS_CASE

