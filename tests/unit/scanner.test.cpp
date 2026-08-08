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

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/version.hpp>
#include <boost/thread.hpp>
#include <boost/uuid/random_generator.hpp>

#include "cryptonote_basic/account.h" // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_config.h"        // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h"      // monero/src
#include "db/chain.test.h"
#include "db/print.test.h"
#include "db/storage.test.h"
#include "device/device_default.hpp" // monero/src
#include "net/zmq.h"                 // monero/src
#include "rpc/client.h"
#include "rpc/daemon_messages.h"     // monero/src
#include "scanner.h"
#include "serialization/json_object.h"
#include "util/transaction.test.h"
#include "wire/error.h"
#include "wire/json/write.h"

namespace
{
  constexpr const std::chrono::seconds message_timeout{3};

  unsigned char* to_bytes(crypto::ec_scalar &scalar) { return &reinterpret_cast<unsigned char&>(scalar); }
  const unsigned char* to_bytes(const crypto::ec_scalar &scalar) { return &reinterpret_cast<const unsigned char&>(scalar); }

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

  epee::byte_slice daemon_pub(const std::vector<cryptonote::transaction> txes)
  {
    epee::byte_stream out;
    const boost::string_ref filter{"json-full-txpool_add:"};
    out.write({filter.data(), filter.size()});
    {
      rapidjson::Writer<epee::byte_stream> writer{out};
      cryptonote::json::toJsonValue(writer, txes);
    }
    return epee::byte_slice{std::move(out)};
  }

  struct join
  {
      boost::thread& thread;
      ~join() { thread.join(); }
  };
   
  void scanner_thread(lws::scanner& scanner, void* ctx, const std::vector<epee::byte_slice>& reply)
  {
    struct stop_
    {
      lws::scanner& scanner;
      ~stop_() { scanner.shutdown(); };
    } stop{scanner};

    lws_test::rpc_thread(ctx, reply);
  }

  void scanner_pub_thread(lws::scanner& scanner, void* ctx, const epee::byte_slice& rpc, const std::vector<epee::byte_slice>& pubs, std::atomic<bool>& pub_ready, const std::atomic<bool>& finished)
  {
    struct stop_
    {
      lws::scanner& scanner;
      ~stop_() { scanner.shutdown(); };
    } stop{scanner};

    std::vector<epee::byte_slice> rpcs;
    rpcs.push_back(rpc.clone());

    lws_test::rpc_pub_thread(ctx, rpcs, pubs, pub_ready, finished);
  }

  namespace webhook
  {
    struct connection;
    struct server
    {
      using tcp = boost::asio::ip::tcp;

      lest::env& lest_env_;
      boost::asio::io_context& io_;
      boost::asio::ip::tcp::acceptor acceptor_;
      std::vector<std::shared_ptr<connection>> callbacks_;
      boost::mutex sync_;
      std::atomic<bool> ready_;

      explicit server(lest::env& lest_env, boost::asio::io_context& io)
        : lest_env_(lest_env),
          io_(io),
          acceptor_(io, tcp::endpoint(tcp::v4(), 0)),
          callbacks_(),
          sync_(),
          ready_(false)
      {}

      ~server();
    };

    struct connection
    {
      std::weak_ptr<server> parent_;
      const std::string response_;
      boost::asio::ip::tcp::socket sock_;
      boost::beast::flat_buffer buffer_;
      boost::optional<boost::beast::http::parser<true, boost::beast::http::string_body>> parser_;
      boost::asio::ip::tcp::endpoint remote_;
      std::size_t count_;
      bool keep_alive_;

      explicit connection(std::shared_ptr<server> parent)
        : parent_(std::move(parent)),
          response_("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n"),
          sock_(parent->io_),
          buffer_(),
          parser_(),
          count_(0),
          keep_alive_(false)
      {}
    };

    server::~server()
    {}

    struct handler_loop : public boost::asio::coroutine
    {
      std::shared_ptr<connection> self_;

      explicit handler_loop(std::shared_ptr<connection> self) noexcept
        : self_(std::move(self))
      {}

      void operator()(boost::system::error_code error = {}, std::size_t = {})
      {
        if (!self_ || error == boost::beast::http::error::end_of_stream)
          return;

        connection& self = *self_;
        const std::shared_ptr<server> parent = self.parent_.lock();
        if (!parent)
          return;

        lest::env& lest_env = parent->lest_env_;

        const boost::lock_guard<boost::mutex> lock{parent->sync_};
        EXPECT(!error);
        BOOST_ASIO_CORO_REENTER(*this)
        {
          for (;;)
          {
            self.parser_.emplace();
            self.parser_->body_limit(10 * 1024 * 1024);

            BOOST_ASIO_CORO_YIELD boost::beast::http::async_read(
              self.sock_, self.buffer_, *self.parser_, std::move(*this)
            );

            ++self.count_;
            BOOST_ASIO_CORO_YIELD boost::asio::async_write(
              self.sock_, boost::asio::buffer(self.response_.data(), self.response_.size()), std::move(*this)
            );
          }
        }
      }
    };

    struct accept_loop : public boost::asio::coroutine
    {
      std::shared_ptr<server> self_;

      explicit accept_loop(std::shared_ptr<server> self) noexcept
        : self_(std::move(self))
      {}

      void operator()(boost::system::error_code error = {})
      {
        if (!self_)
          return;

        server& self = *self_;
        lest::env& lest_env = self.lest_env_;
        const boost::lock_guard<boost::mutex> lock{self.sync_};
        BOOST_ASIO_CORO_REENTER(*this)
        {
          self.acceptor_.listen();
          self.ready_ = true;
          for (;;)
          {
            self.callbacks_.push_back(std::make_shared<connection>(self_));
            BOOST_ASIO_CORO_YIELD self.acceptor_.async_accept(self.callbacks_.back()->sock_, std::move(*this));

            EXPECT(!error);
            boost::asio::post(self.io_, handler_loop{self.callbacks_.back()});
          }
        }
      }
    };
  } // webhook
} // anonymous

namespace lws_test
{
  void rpc_pub_thread(void* ctx, const std::vector<epee::byte_slice>& reply, const std::vector<epee::byte_slice>& pubs, std::atomic<bool>& pub_ready, const std::atomic<bool>& finished)
  {
    try
    {
      struct stop_
      {
        std::atomic<bool>& ready;
        ~stop_() { ready = true; }
      } stop{pub_ready};

      net::zmq::socket pub{};
      net::zmq::socket server{};
      server.reset(zmq_socket(ctx, ZMQ_REP));
      if (!server || zmq_bind(server.get(), lws_test::rpc_rendevous))
      {
        std::cout << "Failed to create ZMQ server" << std::endl;
        return;
      }

      pub.reset(zmq_socket(ctx, ZMQ_PUB));
      if (!pub || zmq_bind(pub.get(), lws_test::pub_rendevous))
      {
        std::cout << "Failed to create ZMQ pub" << std::endl;
        return;
      }

      pub_ready = true;
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

      for (const epee::byte_slice& message : pubs)
      {
        const auto sent = net::zmq::send(message.clone(), pub.get());
        if (!sent)
        {
          std::cout << "Failed to send dummy PUB message: " << sent.error().message() << std::endl;
          return;
        }
      }

      while (!finished)
        boost::this_thread::sleep_for(boost::chrono::milliseconds{10});
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
    std::shared_ptr<lws::mempool> pool{};
    auto rpc = 
      lws::rpc::context::make(lws_test::rpc_rendevous, lws_test::pub_rendevous, {}, {}, std::chrono::minutes{0}, false, true);

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
      lws_test::transaction tx = lws_test::make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      lws_test::transaction tx2 = lws_test::make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);

      lws_test::transaction tx3 = lws_test::make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      lws_test::transaction tx4 = lws_test::make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      //destinations.emplace_back();
      //destinations.back().amount = 1000;
      //destinations.back().addr = keys_subaddr2.m_account_address;
      //destinations.back().is_subaddress = true;

      //transaction tx5 = lws_test::make_tx(lest_env, keys, destinations, 100, true);
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
        scanner.run(std::move(rpc), pool, 1, {}, opts);
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
      lws_test::transaction tx = lws_test::make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      lws_test::transaction tx2 = lws_test::make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);

      lws_test::transaction tx3 = lws_test::make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      lws_test::transaction tx4 = lws_test::make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      destinations.emplace_back();
      destinations.back().amount = 1000;
      destinations.back().addr = keys_subaddr2.m_account_address;
      destinations.back().is_subaddress = true;

      lws_test::transaction tx5 = lws_test::make_tx(lest_env, keys, destinations, 146, true);
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
        scanner.run(std::move(rpc), pool, 1, {}, opts);
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
        EXPECT(outputs.count() == 7);
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
            {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(3)}}}}
          };
          EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
        }
      }
    } //SECTION (lws::scanner::run (lookahead))

    SECTION("lws::scanner::loop mempool pub")
    {
      std::vector<epee::byte_slice> messages{};
      cryptonote::rpc::GetBlocksFast::Response bmessage{};
      bmessage.start_height = std::uint64_t(last_block.id) + 1;
      bmessage.current_height = bmessage.start_height + 1;
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx =
        lws_test::make_miner_tx(lest_env, last_block.id, account, false).tx;

      const std::vector<crypto::hash> hashes{
        last_block.hash,
        cryptonote::get_block_hash(bmessage.blocks.back().block)
      };
      {
        cryptonote::rpc::GetHashesFast::Response hmessage{};

        hmessage.start_height = std::uint64_t(last_block.id);
        hmessage.hashes = hashes;
        hmessage.current_height = hmessage.start_height + boost::size(hashes) - 1;
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

      messages.clear();
      messages.push_back(daemon_pub({bmessage.blocks.back().block.miner_tx}));
      {
        static constexpr const lws::scanner_options opts{
          0, 0, lws::MINIMUM_BLOCK_DEPTH, 10, false, false, false, false
        };

        std::atomic<bool> pub_ready{false};
        std::atomic<bool> finished{false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};

        const epee::byte_slice init = daemon_response(bmessage);
        boost::thread server_thread(&scanner_pub_thread, std::ref(scanner), rpc.zmq_context(), std::cref(init), std::cref(messages), std::ref(pub_ready), std::cref(finished));
        const join on_scope_exit{server_thread};
        struct stop_rpc_server_
        {
          std::atomic<bool>& finished;
          ~stop_rpc_server_() { finished = true; }
        } stop_rpc_server{finished};

        while (!pub_ready)
          boost::this_thread::sleep_for(boost::chrono::milliseconds{10});

        boost::asio::io_context io;
        const auto server = std::make_shared<webhook::server>(lest_env, io);
        boost::asio::post(io, webhook::accept_loop{server});

        boost::thread webhook_thread{[&] { io.run(); server->ready_ = true; }};
        const join on_scope_exit2{webhook_thread};
        struct stop_webhook_
        {
          boost::asio::io_context& io;
          ~stop_webhook_() { io.stop(); }
        } stop_webhook{io};

        while (!server->ready_)
          boost::this_thread::sleep_for(boost::chrono::milliseconds{10});

        {
          const boost::lock_guard<boost::mutex> lock{server->sync_};
          const lws::db::webhook_value event{
            lws::db::webhook_dupsort{0, boost::uuids::random_generator{}()},
            lws::db::webhook_data{
              "http://127.0.0.1:" + std::to_string(server->acceptor_.local_endpoint().port()),
              "",
              0
            }
          };

          EXPECT(db.add_webhook(lws::db::webhook_type::tx_confirmation, account, event));
        }

        boost::thread scanner_thread{[&] { scanner.run(std::move(rpc), pool, 1, {}, opts); }};
        const join on_scope_exit3{scanner_thread};
        struct stop_scanner_
        {
          std::atomic<bool>& finished;
          ~stop_scanner_() { finished = true; }
        } stop_scanner{finished};

        bool done = false;
        const auto start = std::chrono::steady_clock::now();
        while (!done)
        {
          boost::this_thread::sleep_for(boost::chrono::milliseconds{10});
          const boost::lock_guard<boost::mutex> lock{server->sync_};
          done = (server->callbacks_.size() && server->callbacks_.at(0)->count_);
          if (message_timeout <= std::chrono::steady_clock::now() - start)
            break;
        }
        {
          const boost::lock_guard<boost::mutex> lock{server->sync_};
          EXPECT(done);
        }
      }
    }
  } // SETUP
} // LWS_CASE

