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

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <boost/thread/mutex.hpp>
#include <functional>
#include <memory>
#include <string>
#include "byte_slice.h"    // monero/contrib/epee/include
#include "common/expect.h" // monero/src
#include "net/net_ssl.h"   // monero/contrib/epee/include

namespace net { namespace http
{
  struct client_state;
  using server_response_func = void(boost::system::error_code, std::string);

  //! Primarily for webhooks, where the response is (basically) ignored.
  class client
  {
    std::weak_ptr<client_state> state_;
    std::shared_ptr<boost::asio::ssl::context> ssl_;
    boost::mutex sync_;

    expect<void> queue_async(boost::asio::io_context& io, std::string url, epee::byte_slice json_body, std::function<server_response_func> notifier);

  public:
    explicit client(epee::net_utils::ssl_verification_t verify);
    explicit client(std::shared_ptr<boost::asio::ssl::context> ssl);
    ~client();

    const std::shared_ptr<boost::asio::ssl::context>& ssl_context() const noexcept
    { return ssl_; }

    //! Never blocks. Thread safe. \return `success()` if `url` is valid.
    expect<void> post_async(boost::asio::io_context& io, std::string url, epee::byte_slice json_body);

    /*! Never blocks. Thread safe. Calls `notifier` with server response iff
      `success()` is returned.
      \return `success()` if `url` is valid. */
    expect<void> get_async(boost::asio::io_context& io, std::string url, std::function<server_response_func> notifier);
  };
}} // net // http

