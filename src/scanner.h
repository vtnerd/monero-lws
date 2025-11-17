// Copyright (c) 2018-2020, The Monero Project
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

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <string>

#include "db/fwd.h"
#include "db/storage.h"
#include "net/http/client.h"
#include "net/net_ssl.h" // monero/contrib/epee/include
#include "rpc/client.h"
#include "rpc/scanner/fwd.h"
#include "span.h"        // monero/contrib/epee/include

namespace lws
{
  constexpr std::uint64_t MINIMUM_BLOCK_DEPTH = 16;

  struct scanner_options
  {
    std::uint32_t max_subaddresses;
    bool untrusted_daemon;
    bool regtest;
    bool block_depth_threading;
    double split_sync_threads;
    std::uint64_t split_sync_depth;
    std::uint64_t min_block_depth;
    bool balance_new_addresses;
  };

  //! Used in `scan_loop` by server
  class user_data
  {
    db::storage disk_;

  public:
    user_data(db::storage disk)
      : disk_(std::move(disk))
    {}

    user_data(user_data const& rhs)
      : disk_(rhs.disk_.clone())
    {}

    user_data(user_data&& rhs)
      : disk_(std::move(rhs.disk_))
    {}

    /*! Store updated accounts locally (`disk`), and send ZMQ/RMQ/webhook
      events. `users` must be sorted by height (lowest first). */
    static bool store(boost::asio::io_context& io, db::storage& disk, rpc::client& zclient, net::http::client& webhook ,epee::span<const crypto::hash> chain, epee::span<const lws::account> users, epee::span<const db::pow_sync> pow);

    //! `users` must be sorted by height (lowest first)
    bool operator()(boost::asio::io_context& io, rpc::client& zclient, net::http::client& webhook, epee::span<const crypto::hash> chain, epee::span<const lws::account> users, epee::span<const db::pow_sync> pow);
  };

  struct scanner_sync
  {
    boost::asio::io_context io_;
    net::http::client webhooks_;
    std::atomic<bool> stop_;     //!< Stop scanning but do not shutdown
    std::atomic<bool> shutdown_; //!< Exit scanner::run

    explicit scanner_sync(epee::net_utils::ssl_verification_t webhook_verify)
      : io_(), webhooks_(webhook_verify), stop_(false), shutdown_(false)
    {}

    bool is_running() const noexcept { return !stop_ && !shutdown_; }
    bool has_shutdown() const noexcept { return shutdown_; }
    void stop() { stop_ = true; io_.stop(); }
    void shutdown() { shutdown_ = true; stop(); }
  };

  /*! Scans all active `db::account`s. Detects if another process changes
    active list.

    \note Everything except `sync` and `run` is thread-safe. */
  class scanner
  {
    db::storage disk_;
    scanner_sync sync_;
    boost::asio::signal_set signals_; //!< Detect SIGINT requested shutdown

  public:

    //! Register `SIGINT` handler and keep a copy of `disk`
    explicit scanner(db::storage disk, epee::net_utils::ssl_verification_t webhook_verify);
    ~scanner();

    //! Callback for storing user account (typically local lmdb, but perhaps remote rpc)
    using store_func = std::function<bool(boost::asio::io_context&, rpc::client&, net::http::client&, epee::span<const crypto::hash>, epee::span<const lws::account>, epee::span<const db::pow_sync>)>;

    /*! Run _just_ the inner scanner loop while `self.is_running() == true`.
     *
      \throw std::exception on hard errors (shutdown) conditions
      \return True iff `queue` indicates thread now has zero accounts. False
        indicates a soft, typically recoverable error. */
    static bool loop(scanner_sync& self, store_func store, std::optional<db::storage> disk, rpc::client client, std::vector<lws::account> users, rpc::scanner::queue& queue, const scanner_options& opts, const size_t thread_n);
    
    //! Use `client` to sync blockchain data, and \return client if successful.
    expect<rpc::client> sync(rpc::client client, const bool untrusted_daemon = false, const bool regtest = false);

    //! Poll daemon until `shutdown()` is called, using `thread_count` threads.
    void run(rpc::context ctx, std::size_t thread_count, const std::string& server_addr, std::string server_pass, const scanner_options&);

    //! \return True iff `stop()` and `shutdown()` has never been called
    bool is_running() const noexcept { return sync_.is_running(); }

    //! \return True if `shutdown()` has been been called.
    bool has_shutdown() const noexcept { return sync_.has_shutdown(); }

    //! Stop scan threads, but do not shutdown scanner.
    void stop() { sync_.stop(); }

    // Stop scan threads AND shutdown scanner.
    void shutdown() { sync_.shutdown(); }
  };
} // lws
