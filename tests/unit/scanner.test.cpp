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

#include <boost/thread.hpp>
#include <iostream>

#include "db/chain.test.h"
#include "db/storage.test.h"
#include "net/zmq.h"             // monero/src
#include "rpc/client.h"
#include "rpc/daemon_messages.h" // monero/src
#include "scanner.h"
#include "wire/error.h"
#include "wire/json/write.h"

namespace
{
  constexpr const char rendevous[] = "inproc://fake_daemon";
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

  void rpc_thread(void* server, const std::vector<epee::byte_slice>& reply)
  {
    struct stop_
    {
      ~stop_() noexcept { lws::scanner::stop(); }; 
    } stop{};

    try
    {
      for (const epee::byte_slice& message : reply)
      {
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
          const auto request = net::zmq::receive(server, ZMQ_DONTWAIT);
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

        const auto sent = net::zmq::send(message.clone(), server);
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

  struct join
  {
      boost::thread& thread;
      ~join() { thread.join(); }
  }; 
}

LWS_CASE("lws::scanner::sync")
{
  SETUP("lws::rpc::context, ZMQ_REP Server, and lws::db::storage")
  {
    lws::scanner::reset();
    auto rpc = 
      lws::rpc::context::make(rendevous, {}, {}, {}, std::chrono::minutes{0});

    net::zmq::socket server{};
    server.reset(zmq_socket(rpc.zmq_context(), ZMQ_REP));
    EXPECT(server);
    EXPECT(0 <= zmq_bind(server.get(), rendevous));

    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());

    SECTION("Invalid Response")
    {
      const crypto::hash hashes[1] = {
        last_block.hash
      };

      std::vector<epee::byte_slice> messages{};
      messages.push_back(to_json_rpc(1));

      boost::thread server_thread(&rpc_thread, server.get(), std::cref(messages));
      const join on_scope_exit{server_thread};
      EXPECT(!lws::scanner::sync(db.clone(), MONERO_UNWRAP(rpc.connect())));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, hashes);
    }

    SECTION("Chain Update")
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
        boost::thread server_thread(&rpc_thread, server.get(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(lws::scanner::sync(db.clone(), MONERO_UNWRAP(rpc.connect())));
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

        boost::thread server_thread(&rpc_thread, server.get(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(lws::scanner::sync(db.clone(), MONERO_UNWRAP(rpc.connect())));
        lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
      }
    }
  }
}


