// Copyright (c) 2023-2024, The Monero Project
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

#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/utility/string_ref.hpp>
#include <chrono>
#include <string>
#include "byte_slice.h"      // monero/contrib/epee/include
#include "misc_log_ex.h"             // monero/contrib/epee/include
#include "net/http_client.h" // monero/contrib/epee/include
#include "span.h"
#include "wire/json.h"
#include "wire/msgpack.h"

namespace lws { namespace rpc
{
  namespace net = epee::net_utils;

  template<typename T>
  void http_send(net::http::http_simple_client& client, boost::string_ref uri, const T& event, const net::http::fields_list& params, const std::chrono::milliseconds timeout)
  {
    if (uri.empty())
      uri = "/";

    epee::byte_slice bytes{};
    const std::string& url = event.value.second.url;
    const std::error_code json_error = wire::json::to_bytes(bytes, event);
    const net::http::http_response_info* info = nullptr;
    if (json_error)
    {
      MERROR("Failed to generate webhook JSON: " << json_error.message());
      return;
    }

    MINFO("Sending webhook to " << url);
    if (!client.invoke(uri, "POST", std::string{bytes.begin(), bytes.end()}, timeout, std::addressof(info), params))
    {
      MERROR("Failed to invoke http request to  " << url);
      return;
    }

    if (!info)
    {
      MERROR("Failed to invoke http request to  " << url << ", internal error (null response ptr)");
      return;
    }

    if (info->m_response_code != 200 && info->m_response_code != 201)
    {
      MERROR("Failed to invoke http request to  " << url << ", wrong response code: " << info->m_response_code);
      return;
    }
  }

  template<typename T>
  void http_send(const epee::span<const T> events, const std::chrono::milliseconds timeout, net::ssl_verification_t verify_mode)
  {
    if (events.empty())
      return;

    net::http::url_content url{};
    net::http::http_simple_client client{};

    net::http::fields_list params;
    params.emplace_back("Content-Type", "application/json; charset=utf-8");

    for (const auto& event : events)
    {
      if (event.value.second.url.empty() || !net::parse_url(event.value.second.url, url))
      {
        MERROR("Bad URL for webhook event: " << event.value.second.url);
        continue;
      }

      const bool https = (url.schema == "https");
      if (!https && url.schema != "http")
      {
        MERROR("Only http or https connections: " << event.value.second.url);
        continue;
      }

      const net::ssl_support_t ssl_mode = https ?
        net::ssl_support_t::e_ssl_support_enabled : net::ssl_support_t::e_ssl_support_disabled;
      net::ssl_options_t ssl_options{ssl_mode};
      if (https)
        ssl_options.verification = verify_mode;

      if (url.port == 0)
        url.port = https ? 443 : 80;

      client.set_server(url.host, std::to_string(url.port), boost::none, std::move(ssl_options));
      if (client.connect(timeout))
        http_send(client, url.uri, event, params, timeout);
      else
        MERROR("Unable to send webhook to " << event.value.second.url);

      client.disconnect();
    }
  }

  template<typename T>
  struct zmq_index_single
  {
    const std::uint64_t index;
    const T& event;
  };

  template<typename T>
  void write_bytes(wire::writer& dest, const zmq_index_single<T>& self)
  {
    wire::object(dest, WIRE_FIELD(index), WIRE_FIELD(event));
  }

  template<typename T>
  void zmq_send(rpc::client& client, const epee::span<const T> events, const boost::string_ref json_topic, const boost::string_ref msgpack_topic)
  {
    // Each `T` should have a unique count. This is desired.
    struct zmq_order
    {
      std::uint64_t current;
      boost::mutex sync;

      zmq_order()
        : current(0), sync()
      {}
    };

    static zmq_order ordering{};

    //! \TODO monitor XPUB to cull the serialization
    if (!events.empty() && client.has_publish())
    {
      // make sure the event is queued to zmq in order.
      const boost::unique_lock<boost::mutex> guard{ordering.sync};

      for (const auto& event : events)
      {
        const zmq_index_single<T> index{ordering.current++, event};
        MINFO("Sending ZMQ-PUB topics " << json_topic << " and " << msgpack_topic);
        expect<void> result = success();
        if (!(result = client.publish<wire::json>(json_topic, index)))
          MERROR("Failed to serialize+send " << json_topic << " " << result.error().message());
        if (!(result = client.publish<wire::msgpack>(msgpack_topic, index)))
          MERROR("Failed to serialize+send " << msgpack_topic << " " << result.error().message());
      }
    }
  }

  template<typename T>
  void send_webhook(
    rpc::client& client,
    const epee::span<const T> events,
    const boost::string_ref json_topic,
    const boost::string_ref msgpack_topic,
    const std::chrono::seconds timeout,
    epee::net_utils::ssl_verification_t verify_mode)
  {
    http_send(events, timeout, verify_mode);
    zmq_send(client, events, json_topic, msgpack_topic);
  }
}} // lws // rpc
