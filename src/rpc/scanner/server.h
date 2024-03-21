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

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <set>
#include <string>

#include "db/storage.h"
#include "net/net_ssl.h" // monero/contrib/epee/include
#include "rpc/client.h"

namespace lws { namespace rpc { namespace scanner
{
  struct server_connection;
  using ssl_verification_t = epee::net_utils::ssl_verification_t;

  /*!
    \brief Manages remote scanning for the primary daemon.

    \note HTTP and ZMQ were not used because a two-way messaging system were
      needed (basically a REST server on either end). */
  class server
  {
    boost::asio::io_service context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::set<std::weak_ptr<server_connection>, std::owner_less<std::weak_ptr<server_connection>>> scanners_;
    db::storage disk_;
    rpc::client zclient_;
    ssl_verification_t webhook_verify_;

    //! Async acceptor routine
    struct acceptor;

  public:
    static boost::asio::ip::tcp::endpoint get_endpoint(const std::string& address);

    explicit server(const std::string& address, db::storage disk, rpc::client zclient, ssl_verification_t webhook_verify);

    server(const server&) = delete;
    server(server&&) = delete;
    ~server();
    server& operator=(const server&) = delete;
    server& operator=(server&&) = delete;

    //! Process I/O from `monero-lws-scanner` instances.
    expect<void> poll_io();
  };
}}} // lws // rpc // scanner

