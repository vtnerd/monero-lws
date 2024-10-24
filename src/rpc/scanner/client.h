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
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "crypto/hash.h" // monero/src
#include "db/fwd.h"
#include "rpc/scanner/connection.h"
#include "rpc/scanner/queue.h"

namespace lws { namespace rpc { namespace scanner
{
  //! \brief 
  class client : public connection
  {
    const std::vector<std::shared_ptr<queue>> local_;
    const std::string pass_;
    std::size_t next_push_;
    boost::asio::steady_timer connect_timer_;
    const boost::asio::ip::tcp::endpoint server_address_;
    bool connected_;

    struct close;
    class connector;

  public:
    using command = bool(*)(const std::shared_ptr<client>&);

    //! Does not start connection to `address`, see `connect`.
    explicit client(boost::asio::io_context& io, const std::string& address, std::string pass, std::vector<std::shared_ptr<queue>> local);

    client(const client&) = delete;
    client(client&&) = delete;
    ~client();
    client& operator=(const client&) = delete;
    client& operator=(client&&) = delete;

    //! \return Handlers for client commands
    static const std::array<command, 2>& commands() noexcept;

    //! Start a connect loop on `self`.
    static void connect(const std::shared_ptr<client>& self);

    //! Push `users` to local queues. Synchronizes with latest connection.
    static void push_accounts(const std::shared_ptr<client>& self, std::vector<lws::account> users);

    //! Replace `users` on local queues. Synchronizes with latest connection.
    static void replace_accounts(const std::shared_ptr<client>& self, std::vector<lws::account> users);

    //! Send `users` upstream for disk storage
    static void send_update(const std::shared_ptr<client>& self, std::vector<lws::account> users, std::vector<crypto::hash> blocks);

    //! Closes socket and calls stop on `io_context`.
    void cleanup();
  };
}}} // lws // rpc // scanner
