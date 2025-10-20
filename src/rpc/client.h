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

#include <boost/asio/io_context.hpp>
#include <boost/optional/optional.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <zmq.h>

#include "byte_slice.h"    // monero/contrib/epee/include
#include "db/fwd.h"
#include "common/expect.h" // monero/src
#include "net/zmq.h"       // monero/src
#include "rpc/message.h"   // monero/src
#include "rpc/daemon_pub.h"
#include "rpc/rates.h"
#include "util/source_location.h"

namespace net { namespace zmq { struct async_client; }}
namespace lws
{
namespace rpc
{
  namespace detail
  {
    struct context;
  }

  struct rmq_details
  {
    std::string address;
    std::string credentials;
    std::string exchange;
    std::string routing;
  };

  expect<void> parse_response(cryptonote::rpc::Message& parser, std::string msg, source_location loc = {});

  //! Abstraction for ZMQ RPC client. All `const` and `static` methods are thread-safe.
  class client
  {
    std::shared_ptr<detail::context> ctx;
    net::zmq::socket daemon;
    net::zmq::socket daemon_sub;
    net::zmq::socket signal_sub;

    explicit client(std::shared_ptr<detail::context> ctx) noexcept
      : ctx(std::move(ctx)), daemon(), daemon_sub(), signal_sub()
    {}

    //! \return Connection to daemon REQ/REP.
    static expect<net::zmq::socket> make_daemon(const std::shared_ptr<detail::context>& ctx) noexcept;

    //! Expect `response` as the next message payload unless error.
    expect<void> get_response(cryptonote::rpc::Message& response, std::chrono::seconds timeout, source_location loc);

  public:

    static constexpr const char* payment_topic_json() { return "json-full-payment_hook:"; }
    static constexpr const char* payment_topic_msgpack() { return "msgpack-full-payment_hook:"; }

    enum class topic : std::uint8_t
    {
      block = 0, txpool
    };

    //! A client with no connection (all send/receive functions fail).
    explicit client() noexcept
      : ctx(), daemon(), daemon_sub(), signal_sub()
    {}

    static expect<client> make(std::shared_ptr<detail::context> ctx) noexcept;

    client(client&&) = default;
    client(client const&) = delete;

    ~client() noexcept;

    client& operator=(client&&) = default;
    client& operator=(client const&) = delete;

    /*!
      \note `watch_scan_signals()` status is not cloned.
      \note The copy is not cheap - it creates a new ZMQ socket.
      \return A client connected to same daemon as `this`.
    */
    expect<client> clone() const noexcept
    {
      return make(ctx);
    }

    //! \return True if `this` is valid (i.e. not default or moved from).
    explicit operator bool() const noexcept
    {
      return ctx != nullptr;
    }

    //! \return True if an external pub/sub was setup
    bool has_publish() const noexcept;

    //! `wait`, `send`, and `receive` will watch for `raise_abort_scan()`.
    expect<void> watch_scan_signals() noexcept;

    //! Wait for new block announce or internal timeout.
    expect<std::vector<std::pair<topic, std::string>>> wait_for_block();

    using rpc_handler = std::function<expect<void>(std::string&&)>;
    using block_pub_handler = std::function<expect<void>(minimal_chain_pub&&)>;
    using txpool_pub_handler = std::function<expect<void>(full_txpool_pub&&)>;

    //! Loops until an error or shutdown happens, emitting events.
    expect<void> event_loop(
      rpc_handler on_rpc,
      block_pub_handler on_block,
      txpool_pub_handler on_txpool
    );

    //! \return A JSON message for RPC request `M`.
    template<typename M>
    static epee::byte_slice make_message(char const* const name, const M& message)
    {
      return cryptonote::rpc::FullMessage::getRequest(name, message, 0);
    }

    //! \return `async_client` to daemon. Thread safe.
    expect<net::zmq::async_client> make_async_client(boost::asio::io_context& io) const;

    /*!
      Queue `message` for sending to daemon. If the queue is full, wait a
      maximum of `timeout` seconds or until `context::raise_abort_scan` or
      `context::raise_abort_process()` is called.
    */
    expect<void> send(epee::byte_slice message, std::chrono::seconds timeout) noexcept;

    //! Publish `payload` to ZMQ external pub socket. Blocks iff RMQ.
    expect<void> publish(epee::byte_slice payload) const;

    //! Publish `data` after `topic` to ZMQ external pub socket. Blocks iff RMQ.
    template<typename F, typename T>
    expect<void> publish(const boost::string_ref topic, const T& data) const
    {
      epee::byte_stream bytes{};
      bytes.write(topic.data(), topic.size());
      const std::error_code err = F::to_bytes(bytes, data);
      if (err)
        return err;
      return publish(epee::byte_slice{std::move(bytes)});
    }

    //! \return Next available RPC message response from server
    expect<std::string> get_message(std::chrono::seconds timeout);

    //! \return RPC response `M`, waiting a max of `timeout` seconds. Log errors as from `loc`.
    template<typename M>
    expect<M> receive(const std::chrono::seconds timeout, const source_location loc = {})
    {
      M response{};
      MONERO_CHECK(get_response(response, timeout, loc));
      return response;
    }

    /*! Never blocks for I/O - that is performed on another thread.
      \return Recent exchange rates. */
    expect<rates> get_rates() const;
  };

  //! Owns ZMQ context, and ZMQ PUB socket for signalling child `client`s.
  class context
  {
    std::shared_ptr<detail::context> ctx;

    explicit context(std::shared_ptr<detail::context> ctx)
      : ctx(std::move(ctx))
    {}

  public:
    /*! Use `daemon_addr` for call child client objects.

      \throw std::bad_alloc if internal `shared_ptr` allocation failed.
      \throw std::system_error if any ZMQ errors occur.

      \note All errors are exceptions; no recovery can occur.

      \param daemon_addr Location of ZMQ enabled `monerod` RPC.
      \param pub_addr Bind location for publishing ZMQ events.
      \param rmq_info Required information for RMQ publishing (if enabled)
      \param rates_interval Frequency to retrieve exchange rates. Set value to
        `<= 0` to disable exchange rate retrieval.
      \param True if additional size constraints should be placed on
        daemon messages
    */
    static context make(std::string daemon_addr, std::string sub_addr, std::string pub_addr, rmq_details rmq_info, std::chrono::minutes rates_interval, const bool untrusted_daemon);

    context(context&&) = default;
    context(context const&) = delete;

    //! Calls `raise_abort_process()`. Clients can safely destruct later.
    ~context() noexcept;

    context& operator=(context&&) = default;
    context& operator=(context const&) = delete;

    // Do not create clone method, only one of these should exist right now.

    // \return zmq context pointer (for testing).
    void* zmq_context() const;

    //! \return The full address of the monerod ZMQ daemon.
    std::string const& daemon_address() const;

    //! \return The full address of the monerod PUB port.
    std::string const& pub_address() const;

    //! \return Exchange rate checking interval
    std::chrono::minutes cache_interval() const; 

    //! \return Client connection. Thread-safe.
    expect<client> connect() const noexcept
    {
      return client::make(ctx);
    }

    /*!
      All block `client::send`, `client::receive`, and `client::wait` calls
      originating from `this` object AND whose `watch_scan_signal` method was
      invoked, will immediately return with `lws::error::kSignlAbortScan`. This
      is NOT signal-safe NOR signal-safe NOR thread-safe.
    */
    expect<void> raise_abort_scan() noexcept;

    /*!
      All blocked `client::send`, `client::receive`, and `client::wait` calls
      originating from `this` object will immediately return with
      `lws::error::kSignalAbortProcess`. This call is NOT signal-safe NOR
      thread-safe.
    */
    expect<void> raise_abort_process() noexcept;

    /*!
      Retrieve exchange rates, if enabled. Thread-safe. All clients will see
      new rates once completed.

      \return `success()` if HTTP GET was queued. */
    expect<void> retrieve_rates_async(boost::asio::io_context& io);
  };
} // rpc
} // lws
