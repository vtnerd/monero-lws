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
#include "rpc/client.h"
#include "misc_log_ex.h"

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
    std::atomic<bool> auth_seen{false};
    handler()
      : epee::http_server_impl_base<handler, context>()
    {}

    virtual bool
    handle_http_request(const enet::http::http_request_info& query, enet::http::http_response_info& response, context&)
    override final
    {
      if (query.m_request_head.find("Authorization: Apikey TEST_SENTINEL") != std::string::npos)
        auth_seen = true;
      if (query.m_URI == "/")
        response.m_response_code = 404;
      else if (query.m_URI == "/unauthorized")
        response.m_response_code = 401;
      else if (query.m_URI == "/limited")
        response.m_response_code = 429;
      else if (query.m_URI == "/created")
        response.m_response_code = 201;
      else
        response.m_response_code = 200;
      if (query.m_URI == "/rates" || query.m_URI == "/rates2")
        response.m_body = std::string{R"({"AUD":1,"BRL":1,"BTC":1,"CAD":1,"CHF":1,"CNY":1,"EUR":1,"GBP":1,"HKD":1,"INR":1,"JPY":1,"KRW":1,"MXN":1,"NOK":1,"NZD":1,"SEK":1,"SGD":1,"TRY":1,"USD":)"} + (query.m_URI == "/rates2" ? "3" : "2") + R"(,"RUB":1,"ZAR":1})";
      else if (query.m_URI == "/invalid-json")
        response.m_body = "{\"USD\":";
      else if (query.m_URI == "/partial")
        response.m_body = "{\"USD\":5}";
      else
        response.m_body = query.m_URI;
      return true;
    }
  };
}

LWS_CASE("rpc::rates HTTP callback")
{
  const auto invalid_key = []
  {
    return lws::rpc::context::make(
      "tcp://127.0.0.1:19998", "", "", {}, std::chrono::minutes{60}, false, false, "bad\r\nheader"
    );
  };
  EXPECT_THROWS(invalid_key());
  mlog_configure("", true);
  mlog_set_log_level(2);
  boost::asio::io_context io;
  handler server{};
  server.init(&crypto::generate_random_bytes_thread_safe, std::to_string(server_port));
  server.run(1, false);
  lws::rpc::context rates = lws::rpc::context::make(
    "tcp://127.0.0.1:19998", "", "", {}, std::chrono::minutes{60}, false, false, "TEST_SENTINEL"
  );
  auto reader = rates.connect();
  EXPECT(bool(reader));
  const std::string base = "http://127.0.0.1:" + std::to_string(server_port);
  const auto fetch = [&] (const std::string& path)
  {
    EXPECT(bool(rates.retrieve_rates_async(io, base + path)));
    io.run();
    io.restart();
  };

  fetch("/rates");
  const auto initial = reader->get_rates();
  EXPECT(bool(initial));
  EXPECT(initial->USD == 2);
  fetch("/unauthorized");
  const auto retained = reader->get_rates();
  EXPECT(bool(retained));
  EXPECT(retained->USD == initial->USD);
  fetch("/limited");
  fetch("/invalid-json");
  fetch("/partial");
  EXPECT(bool(rates.retrieve_rates_async(io, "http://127.0.0.1:" + std::to_string(invalid_server_port))));
  io.run();
  io.restart();
  const auto retained_after_errors = reader->get_rates();
  EXPECT(bool(retained_after_errors));
  EXPECT(retained_after_errors->USD == initial->USD);
  fetch("/rates2");
  const auto refreshed = reader->get_rates();
  EXPECT(bool(refreshed));
  EXPECT(refreshed->USD == 3);
  EXPECT(bool(server.auth_seen));
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
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 200);
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
      EXPECT(!server.auth_seen);
    }

    SECTION("GET 200 OK Twice")
    {
      std::atomic<unsigned> done = 0;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 200);
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
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 404);
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

    SECTION("GET 401 UNAUTHORIZED")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 401);
        EXPECT(body.empty());
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/unauthorized", handler
      );

      while (!done)
      {
        io.run_one();
        io.restart();
      }
    }

    SECTION("GET 429 TOO MANY REQUESTS")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 429);
        EXPECT(body.empty());
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/limited", handler
      );

      while (!done)
      {
        io.run_one();
        io.restart();
      }
    }

    SECTION("GET 201 CREATED")
    {
      std::atomic<bool> done = false;
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(!error);
        EXPECT(status == 201);
        EXPECT(body == "/created");
        done = true;
      };
      client.get_async(
        io, "http://127.0.0.1:" + std::to_string(server_port) + "/created", handler
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
      const auto handler = [&done, &lest_env] (boost::system::error_code error, unsigned status, std::string body)
      {
        EXPECT(error == boost::asio::error::connection_refused);
        EXPECT(status == 0);
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
