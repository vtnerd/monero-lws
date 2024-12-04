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

#include <atomic>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <string>
#include "crypto/crypto.h"             // monero/src
#include "net/http/client.h"
#include "net/http_server_impl_base.h" // monero/contrib/epee/include

namespace
{
  constexpr const std::uint16_t server_port = 10000;
  constexpr const std::uint16_t invalid_server_port = 10001;

  namespace enet = epee::net_utils;
  struct context : enet::connection_context_base
  {
    context()
      : enet::connection_context_base()
    {}
  };

  struct handler : epee::http_server_impl_base<handler, context>
  {
    handler()
      : epee::http_server_impl_base<handler, context>()
    {}

    virtual bool
    handle_http_request(const enet::http::http_request_info& query, enet::http::http_response_info& response, context&)
    override final
    {
      if (query.m_URI == "/")
        response.m_response_code = 404;
      else
        response.m_response_code = 200;
      response.m_body = query.m_URI;
      return true;
    }
  };
}

LWS_CASE("net::http::client")
{

  boost::asio::io_context io;
  handler server{};
  server.init(&crypto::generate_random_bytes_thread_safe, std::to_string(server_port));
  server.run(1, false);

  SETUP("server and client")
  {  
    net::http::client client{epee::net_utils::ssl_verification_t::none};

    SECTION("GET 200 OK")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, std::string body)
      {
        EXPECT(!error);
        EXPECT(body == "/some_endpoint");
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/some_endpoint", handler
      );

      while (!done)
      {
        io.run_one();
        io.restart();
      }
    }

    SECTION("GET 200 OK Twice")
    {
      std::atomic<unsigned> done = 0;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, std::string body)
      {
        EXPECT(!error);
        EXPECT(body == "/some_endpoint");
        ++done;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/some_endpoint", handler
      );
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/some_endpoint", handler
      );

      while (done != 2)
      {
        io.run_one();
        io.restart();
      }
    }

    SECTION("GET 404 NOT FOUND")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, std::string body)
      {
        EXPECT(error == boost::asio::error::operation_not_supported);
        EXPECT(body.empty());
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port), handler
      );

      while (!done)
      {
        io.run_one();
        io.restart();
      }
    }

    SECTION("GET (Invalid server address)")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, std::string body)
      {
        EXPECT(error == boost::asio::error::connection_refused);
        EXPECT(body.empty());
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(invalid_server_port), handler
      );

      while (!done)
      {
        io.run_one();
        io.restart();
      }
    }
  }
}
