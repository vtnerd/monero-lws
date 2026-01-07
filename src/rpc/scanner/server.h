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

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <set>
#include <sodium/crypto_pwhash.h>
#include <string>

#include "db/fwd.h"
#include "db/storage.h"
#include "net/http/client.h"
#include "net/net_ssl.h" // monero/contrib/epee/include
#include "rpc/client.h"
#include "rpc/scanner/queue.h"

namespace lws { namespace rpc { namespace scanner
{ 
  //! Checking frequency for local user db changes 
  constexpr const std::chrono::seconds account_poll_interval{10};

  using ssl_verification_t = epee::net_utils::ssl_verification_t;
  struct server_connection;

  /*!
    \brief Manages local and remote scanning for the primary daemon.

    \note HTTP and ZMQ were not used because a two-way messaging system were
      needed (basically a REST server on either end). */
  class server
  {
    boost::asio::io_context::strand strand_;
    boost::asio::steady_timer check_timer_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::set<std::weak_ptr<server_connection>, std::owner_less<std::weak_ptr<server_connection>>> remote_;
    std::vector<std::shared_ptr<queue>> local_;
    std::vector<db::account_id> active_;
    db::storage disk_;
    rpc::client zclient_;
    net::http::client webhook_;
    db::cursor::accounts accounts_cur_;
    std::size_t next_thread_;
    std::array<unsigned char, 32> pass_hashed_;
    std::array<unsigned char, crypto_pwhash_SALTBYTES> pass_salt_;
    bool stop_;
    bool balance_new_addresses_;

    //! Async acceptor routine
    class acceptor;
    struct check_users;
 
    //! Reset `local_` and `remote_` scanners. Must be called in `strand_`.
    void do_replace_users();

    //! Stop all async operations
    void do_stop();

  public:
    static boost::asio::ip::tcp::endpoint get_endpoint(const std::string& address);

    explicit server(boost::asio::io_context& io, db::storage disk, rpc::client zclient, std::vector<std::shared_ptr<queue>> local, std::vector<db::account_id> active, std::shared_ptr<boost::asio::ssl::context> ssl, bool balance_new_addresses = false);

    server(const server&) = delete;
    server(server&&) = delete;
    ~server() noexcept;
    server& operator=(const server&) = delete;
    server& operator=(server&&) = delete;

    //! \return True if `pass` matches expected
    bool check_pass(const std::string& pass) const noexcept;

    void compute_hash(std::array<unsigned char, 32>& out, const std::string& pass) const noexcept;

    //! Start listening for incoming connections on `address`.
    static void start_acceptor(const std::shared_ptr<server>& self, const std::string& address, std::string pass);

    //! Start timed checks of local DB for change in user state
    static void start_user_checking(const std::shared_ptr<server>& self);

    //! Replace users/accounts on all local and remote threads
    static void replace_users(const std::shared_ptr<server>& self);

    //! Update `users` information on local DB
    static void store(const std::shared_ptr<server>& self, std::vector<lws::account> users, std::vector<crypto::hash> blocks);

    //! Stop a running instance of all operations
    static void stop(const std::shared_ptr<server>& self);
  };
}}} // lws // rpc // scanner
