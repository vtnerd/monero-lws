// Copyright (c) 2026, The Monero Project
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
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <chrono>
#include <cstdint>
#include <system_error>

#include "db/fwd.h"
#include "rpc/fwd.h"
#include "wire/fwd.h"

namespace lws { namespace rpc { namespace feed
{
  using request =
    boost::beast::http::request<boost::beast::http::string_body>;

  enum class error { none = 0, missed_messages, protocol, queue_max };

  // Do not change order or value as these are exported to clients (append)
  enum class status : std::uint16_t
  {
    unspecified_error = 0,
    account_not_found,
    bad_address,
    bad_view_key,
    blockchain_reorg,
    daemon_unresponsive,
    parse_error,
    protocol_error,
    queue_error,
    schema_error
  };
  WIRE_AS_INTEGER(status);

  status map(std::error_code error) noexcept;

  const char* get_string(feed::error value) noexcept;
  std::error_category const& error_category() noexcept;
  inline std::error_code make_error_code(feed::error value) noexcept
  {
    return std::error_code{int(value), error_category()};
  }

  bool start(boost::asio::ip::tcp::socket&& sock, boost::asio::io_context& io, const lws::rpc::client& client, const request& req, const lws::db::storage& disk, std::chrono::seconds timeout);
  bool start(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>&& sock, boost::asio::io_context& io, const lws::rpc::client& client, const request& req, const lws::db::storage& disk, std::chrono::seconds idle_timeout);
}}} // lws // rpc // feed

namespace std
{
  template<>
  struct is_error_code_enum<::lws::rpc::feed::error>
    : true_type
  {};
}
