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

#include <boost/asio/io_context.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/utility/string_ref.hpp>
#include <chrono>
#include <string>
#include "byte_slice.h"      // monero/contrib/epee/include
#include "misc_log_ex.h"             // monero/contrib/epee/include
#include "net/http/client.h"
#include "span.h"
#include "wire/json.h"
#include "wire/msgpack.h"

namespace lws { namespace rpc
{
template<typename T>
void http_async(boost::asio::io_context& io, net::http::client& client, const epee::span<const T> events)
{
  for (const auto& event : events)
  {
    if (event.value.second.url != "zmq")
    {
      epee::byte_slice bytes{};
      const std::error_code json_error = wire::json::to_bytes(bytes, event);
      if (!json_error)
      {
        MINFO("Sending webhook to " << event.value.second.url);
        const expect<void> rc =
          client.post_async(io, event.value.second.url, std::move(bytes));
        if (!rc)
          MERROR("Failed to send HTTP webhook to " << event.value.second.url << ": " << rc.error().message());
      }
      else
        MERROR("Failed to generate webhook JSON: " << json_error.message());
    }
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
void zmq_send(const rpc::client& client, const epee::span<const T> events, const boost::string_ref json_topic, const boost::string_ref msgpack_topic)
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
  void send_webhook_async(
    boost::asio::io_context& io,
    const rpc::client& client,
    net::http::client& http_client,
    const epee::span<const T> events,
    const boost::string_ref json_topic,
    const boost::string_ref msgpack_topic)
  {
    http_async(io, http_client, events);

    // ZMQ PUB sockets never block, but RMQ does. No easy way around this.
    zmq_send(client, events, json_topic, msgpack_topic);
  }
}} // lws // rpc
