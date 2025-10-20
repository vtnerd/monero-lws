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

#include "client.h"

#include <array>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/utility/string_ref.hpp>
#include <cassert>
#include <system_error>

#include "common/error.h"    // monero/contrib/epee/include
#include "db/account.h"
#include "error.h"
#include "misc_log_ex.h"     // monero/contrib/epee/include
#include "net/http_client.h" // monero/contrib/epee/include
#include "net/zmq.h"         // monero/src
#include "net/zmq_async.h"
#include "scanner.h"
#include "serialization/json_object.h" // monero/src
#include "wire/json/write.h"
#if MLWS_RMQ_ENABLED
  #include <amqp.h>
  #include <amqp_tcp_socket.h>
#endif

namespace lws
{
  // Not in `rates.h` - defaulting to JSON output seems odd
  std::ostream& operator<<(std::ostream& out, lws::rates const& src)
  {
    wire::json_stream_writer dest{out};
    lws::write_bytes(dest, src);
    dest.finish();
    return out;
  }

namespace rpc
{
  namespace http = epee::net_utils::http;

  namespace
  {
    constexpr const char signal_endpoint[] = "inproc://signal";
    constexpr const char account_endpoint[] = "inproc://account"; // append integer every new `account_push`
    constexpr const char abort_scan_signal[] = "SCAN";
    constexpr const char abort_process_signal[] = "PROCESS";
    constexpr const char minimal_chain_topic[] = "json-minimal-chain_main";
    constexpr const char full_txpool_topic[] = "json-full-txpool_add";
    constexpr const int daemon_zmq_linger = 0;
    constexpr const int account_zmq_linger = 0;
    constexpr const std::int64_t max_msg_sub = 10 * 1024 * 1024;  // 50 MiB
    constexpr const std::int64_t max_msg_req = 350 * 1024 * 1024; // 350 MiB
    constexpr const std::chrono::seconds chain_poll_timeout{20};
    constexpr const std::chrono::minutes chain_sub_timeout{4};

    struct terminate
    {
      void operator()(void* ptr) const noexcept
      {
        if (ptr)
        {
          while (zmq_term(ptr))
          {
            if (zmq_errno() != EINTR)
              break;
          }
        }
      }
    };
    using zcontext = std::unique_ptr<void, terminate>;

#ifdef MLWS_RMQ_ENABLED
    constexpr const unsigned rmq_channel = 1;
    struct rdestroy
    {
      void operator()(amqp_connection_state_t ptr) const noexcept
      {
        if (ptr)
        {
          amqp_channel_close(ptr, rmq_channel, AMQP_REPLY_SUCCESS);
          amqp_connection_close(ptr, AMQP_REPLY_SUCCESS);
          amqp_destroy_connection(ptr);
        }
      }
    };
    struct rcontext
    {
      using connection = std::unique_ptr<amqp_connection_state_t_, rdestroy>;

      explicit rcontext(rmq_details&& info)
        : conn(), info(std::move(info))
      {}

      bool is_available() const noexcept { return conn != nullptr; }

      bool connect()
      {
        if (info.address.empty())
          return true;

        epee::net_utils::http::url_content url{};
        if (!epee::net_utils::parse_url(info.address, url))
          MONERO_THROW(error::configuration, "Invalid URL spec given for RMQ");
        if (url.port == 0)
          MONERO_THROW(error::configuration, "No port specified for RMQ");
        if (url.uri.empty())
          url.uri = "/";

        std::string user;
        std::string pass;
        boost::regex expression{"(\\w+):(\\w+)"};
        boost::smatch matcher;
        if (boost::regex_search(info.credentials, matcher, expression))
        {
           user = matcher[1];
           pass = matcher[2];
        }

        conn.reset(amqp_new_connection());
        if (!conn)
          MONERO_THROW(error::rmq_failure, "Failed to create new RMQ connection");
        const auto socket = amqp_tcp_socket_new(conn.get());
        if (!socket)
          MONERO_THROW(error::rmq_failure, "Unable to create RMQ socket");

        int status = amqp_socket_open(socket, url.host.c_str(), url.port);
        if (status != 0)
        {
          MERROR("Unable to open RMQ socket: " << status);
          conn.reset();
          return false;
        }

        if (!user.empty() || !pass.empty())
        {
          if (amqp_login(conn.get(), url.uri.c_str(), 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, user.c_str(), pass.c_str()).reply_type != AMQP_RESPONSE_NORMAL)
          {
            MERROR("Failure to login RMQ socket");
            conn.reset();
            return false;
          }
        }
        if (amqp_channel_open(conn.get(), rmq_channel) == nullptr)
        {
          MERROR("Unable to open RMQ channel");
          conn.reset();
          return false;
        }

        if (amqp_get_rpc_reply(conn.get()).reply_type != AMQP_RESPONSE_NORMAL)
        {
          MERROR("Failed receiving channel open reply");
          conn.reset();
          return false;
        }

        MINFO("Connected to RMQ server " << url.host << ":" << url.port);
        return true;
      }

      connection conn;
      const rmq_details info;
    };
#else // !MLWS_RMQ_ENABLED

    struct rcontext
    {
      constexpr explicit rcontext(const rmq_details&)
      {}

      static constexpr bool is_available() noexcept { return false; }
      static constexpr bool connect() noexcept { return true; }
    };
#endif

    expect<void> do_wait(void* daemon, void* signal_sub, short events, std::chrono::milliseconds timeout) noexcept
    {
      if (timeout <= std::chrono::seconds{0})
        return {lws::error::daemon_timeout};

      zmq_pollitem_t items[2] {
        {daemon, 0, short(events | ZMQ_POLLERR), 0},
        {signal_sub, 0, short(ZMQ_POLLIN | ZMQ_POLLERR), 0}
      };

      for (;;)
      {
        const auto start = std::chrono::steady_clock::now();
        const int ready = zmq_poll(items, 2, timeout.count());
        const auto end = std::chrono::steady_clock::now();
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(start - end);
        timeout -= std::min(spent, timeout);

        if (ready == 0)
          return {lws::error::daemon_timeout};
        if (0 < ready)
          break;
        const int err = zmq_errno();
        if (err != EINTR)
          return ::net::zmq::make_error_code(err);
      }
      if (items[0].revents)
        return success();

      char buf[1];
      MONERO_ZMQ_CHECK(zmq_recv(signal_sub, buf, 1, 0));

      switch (buf[0])
      {
      case 'P':
        return {lws::error::signal_abort_process};
      case 'S':
        return {lws::error::signal_abort_scan};
      default:
        break;
      }
      return {lws::error::signal_unknown};
    }

    template<std::size_t N>
    expect<void> do_signal(void* signal_pub, const char (&signal)[N]) noexcept
    {
      MONERO_ZMQ_CHECK(zmq_send(signal_pub, signal, sizeof(signal) - 1, 0));
      return success();
    }

    template<std::size_t N>
    expect<void> do_subscribe(void* signal_sub, const char (&signal)[N]) noexcept
    {
      MONERO_ZMQ_CHECK(zmq_setsockopt(signal_sub, ZMQ_SUBSCRIBE, signal, sizeof(signal) - 1));
      return success();
    }

    template<typename T>
    expect<void> do_set_option(void* sock, const int option, const T value) noexcept
    {
      MONERO_ZMQ_CHECK(zmq_setsockopt(sock, option, std::addressof(value), sizeof(value)));
      return success();
    }
  } // anonymous

  namespace detail
  {
    struct context
    {
      explicit context(zcontext comm, net::zmq::socket signal_pub, net::zmq::socket external_pub, rcontext rmq_src, std::string daemon_addr, std::string sub_addr, std::chrono::minutes interval, bool untrusted_daemon)
        : comm(std::move(comm))
        , signal_pub(std::move(signal_pub))
        , external_pub(std::move(external_pub))
        , rmq(std::move(rmq_src))
        , daemon_addr(std::move(daemon_addr))
        , sub_addr(std::move(sub_addr))
        , rates_conn(epee::net_utils::ssl_verification_t::system_ca)
        , cache_time()
        , cache_interval(interval)
        , cached{}
        , sync_pub()
        , sync_rates()
        , untrusted_daemon(untrusted_daemon)
        , rates_running()
      {
        rates_running.clear();
        if (!rmq.connect())
          MONERO_THROW(error::configuration, "RMQ misconfigured");
      }

      zcontext comm;
      net::zmq::socket signal_pub;
      net::zmq::socket external_pub;
      rcontext rmq;
      const std::string daemon_addr;
      const std::string sub_addr;
      net::http::client rates_conn;
      std::chrono::steady_clock::time_point cache_time;
      const std::chrono::minutes cache_interval;
      rates cached;
      boost::mutex sync_pub;
      boost::mutex sync_rates;
      const bool untrusted_daemon;
      std::atomic_flag rates_running;
    };
  } // detail

  expect<void> parse_response(cryptonote::rpc::Message& parser, std::string msg, source_location loc)
  {
    try
    {
      cryptonote::rpc::FullMessage fm{std::move(msg)};
      const cryptonote::rpc::error json_error = fm.getError();
      if (!json_error.use)
      {
        parser.fromJson(fm.getMessage());
        return success();
      }

      MERROR("Server returned RPC error: " << json_error.message << " with code " << json_error.code << " called from " << loc);
    }
    catch (const cryptonote::json::JSON_ERROR& error)
    {
      MERROR("Failed to parse json response: " << error.what() << " called from " << loc);
    }

    return {lws::error::bad_daemon_response};
  }

  expect<net::zmq::socket> client::make_daemon(const std::shared_ptr<detail::context>& ctx) noexcept
  {
    assert(ctx != nullptr);

    net::zmq::socket daemon{zmq_socket(ctx->comm.get(), ZMQ_REQ)};

    if (daemon.get() == nullptr)
      return net::zmq::get_error_code();
    MONERO_CHECK(do_set_option(daemon.get(), ZMQ_LINGER, daemon_zmq_linger));
    if (ctx->untrusted_daemon)
      MONERO_CHECK(do_set_option(daemon.get(), ZMQ_MAXMSGSIZE, max_msg_req));
    MONERO_ZMQ_CHECK(zmq_connect(daemon.get(), ctx->daemon_addr.c_str()));

    return daemon;
  }

  expect<void> client::get_response(cryptonote::rpc::Message& response, const std::chrono::seconds timeout, const source_location loc)
  {
    expect<std::string> message = get_message(timeout);
    if (!message)
      return message.error();
    return parse_response(response, std::move(*message), loc);
  }

  expect<std::string> client::get_message(std::chrono::seconds timeout)
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(daemon != nullptr);
    assert(signal_sub != nullptr);

    expect<std::string> msg{common_error::kInvalidArgument};
    while (!(msg = net::zmq::receive(daemon.get(), ZMQ_DONTWAIT)))
    {
      if (msg != net::zmq::make_error_code(EAGAIN))
        break;

      MONERO_CHECK(do_wait(daemon.get(), signal_sub.get(), ZMQ_POLLIN, timeout));
      timeout = std::chrono::seconds{0};
    }
    // std::string move constructor is noexcept
    return msg;
  }

  expect<client> client::make(std::shared_ptr<detail::context> ctx) noexcept
  {
    MONERO_PRECOND(ctx != nullptr);

    client out{std::move(ctx)};

    expect<net::zmq::socket> daemon = make_daemon(out.ctx);
    if (!daemon)
      return daemon.error();
    out.daemon = std::move(*daemon);

    if (!out.ctx->sub_addr.empty())
    {
      out.daemon_sub.reset(zmq_socket(out.ctx->comm.get(), ZMQ_SUB));
      if (out.daemon_sub.get() == nullptr)
        return net::zmq::get_error_code();

      if (out.ctx->untrusted_daemon)
        MONERO_CHECK(do_set_option(out.daemon_sub.get(), ZMQ_MAXMSGSIZE, max_msg_sub));
      MONERO_ZMQ_CHECK(zmq_connect(out.daemon_sub.get(), out.ctx->sub_addr.c_str()));
      MONERO_CHECK(do_subscribe(out.daemon_sub.get(), minimal_chain_topic));
      MONERO_CHECK(do_subscribe(out.daemon_sub.get(), full_txpool_topic));
    }

    out.signal_sub.reset(zmq_socket(out.ctx->comm.get(), ZMQ_SUB));
    if (out.signal_sub.get() == nullptr)
      return net::zmq::get_error_code();
    MONERO_ZMQ_CHECK(zmq_connect(out.signal_sub.get(), signal_endpoint));

    MONERO_CHECK(do_subscribe(out.signal_sub.get(), abort_process_signal));
    return {std::move(out)};
  }

  client::~client() noexcept
  {}

  bool client::has_publish() const noexcept
  {
    return ctx && (ctx->external_pub || ctx->rmq.is_available());
  }

  expect<void> client::watch_scan_signals() noexcept
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(signal_sub != nullptr);
    return do_subscribe(signal_sub.get(), abort_scan_signal);
  }

  expect<std::vector<std::pair<client::topic, std::string>>> client::wait_for_block()
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(daemon != nullptr);
    assert(signal_sub != nullptr);

    if (daemon_sub == nullptr)
    {
      MONERO_CHECK(do_wait(daemon.get(), signal_sub.get(), 0, chain_poll_timeout));
      return {lws::error::daemon_timeout};
    }

    {
      const expect<void> ready = do_wait(daemon_sub.get(), signal_sub.get(), ZMQ_POLLIN, chain_sub_timeout);
      if (!ready)
      {
        if (ready == lws::error::daemon_timeout)
          MWARNING("ZeroMQ Pub/Sub chain timeout, check connection settings");
        return ready.error();
      }
    }

    std::vector<std::pair<topic, std::string>> messages{};
    for (; /*every message */ ;)
    {
      expect<std::string> pub = net::zmq::receive(daemon_sub.get(), ZMQ_DONTWAIT);
      if (!pub)
      {
        if (pub == net::zmq::make_error_code(EAGAIN))
          return {std::move(messages)};
      	return pub.error();
      }
      if (pub->size() < 5)
        break; // for loop

      switch (pub->at(5))
      {
        case 'm': // json-minimal-chain_main
          if (boost::string_ref{*pub}.starts_with(minimal_chain_topic))
          {
            pub->erase(0, sizeof(minimal_chain_topic));
            messages.emplace_back(topic::block, std::move(*pub));
          }
          else
            MWARNING("Unexpected pub/sub message");
          break;
        case 'f': // json-full-txpool_add
          if (boost::string_ref{*pub}.starts_with(full_txpool_topic))
          {
            pub->erase(0, sizeof(full_txpool_topic));
            messages.emplace_back(topic::txpool, std::move(*pub));
          }
          else
            MWARNING("Unexpected pub/sub message");
          break;
        default:
          break;
      }
    } // for every message
    return {lws::error::bad_daemon_response};
  }

  expect<void> client::event_loop(
    rpc_handler on_rpc,
    block_pub_handler on_block,
    txpool_pub_handler on_txpool)
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(signal_sub != nullptr);

    std::array<zmq_pollitem_t, 3> poll_items{};
    std::size_t count = 0;

    poll_items[count++] = {signal_sub.get(), 0, short(ZMQ_POLLIN | ZMQ_POLLERR), 0};
    if (daemon != nullptr)
      poll_items[count++] = {daemon.get(), 0, short(ZMQ_POLLIN | ZMQ_POLLERR), 0};
    if (daemon_sub != nullptr)
      poll_items[count++] = {daemon_sub.get(), 0, short(ZMQ_POLLIN | ZMQ_POLLERR), 0};

    while (true)
    {
      const int ready = zmq_poll(poll_items.data(), count, -1);
      if (ready < 0)
      {
        const int err = zmq_errno();
        if (err == EINTR)
          continue;
        MERROR("Failed polling sockets: " << err);
        return net::zmq::make_error_code(err);
      }

      // check for abort messages:
      if (poll_items[0].revents & (ZMQ_POLLIN | ZMQ_POLLERR))
      {
        char buf[1];
        MONERO_ZMQ_CHECK(zmq_recv(signal_sub.get(), buf, 1, 0));
        switch (buf[0])
        {
        case 'P':
          return {lws::error::signal_abort_process};
        case 'S':
          return {lws::error::signal_abort_scan};
        default:
          return {lws::error::signal_unknown};
        }
      }

      // check for rpc responses:
      if (daemon && (poll_items[1].revents & (ZMQ_POLLIN | ZMQ_POLLERR)))
      {
        auto json = net::zmq::receive(daemon.get(), ZMQ_DONTWAIT);
        if (!json)
        {
          if (json == net::zmq::make_error_code(EFSM))
          {
            MERROR("FSM again: " << json.error());
            break;
          }
          if (json == net::zmq::make_error_code(EAGAIN) || json == net::zmq::make_error_code(EFSM))
            break;
          MERROR("Failed reading RPC response: " << json.error());
          return json.error();
        } else if (on_rpc)
          MONERO_CHECK(on_rpc(std::move(*json)));
      }

      // check for PUB notificatons:
      std::size_t sub_index = daemon ? 2 : 1;
      if (daemon_sub && (poll_items[sub_index].revents & (ZMQ_POLLIN | ZMQ_POLLERR)))
      {
        while (true)
        {
          auto json = net::zmq::receive(daemon_sub.get(), ZMQ_DONTWAIT);
          if (!json)
          {
            if (json == net::zmq::make_error_code(EAGAIN))
              break;
            MERROR("Failed reading pub message: " << json.error());
            return json.error();
          }

          if (boost::string_ref{*json}.starts_with(minimal_chain_topic))
          {
            json->erase(0, sizeof(minimal_chain_topic));
            auto parsed = rpc::minimal_chain_pub::from_json(std::move(*json));
            if (!parsed)
            {
              MERROR("Failed parsing chain pub: " << parsed.error().message());
              return parsed.error();
            }
            if (on_block)
              MONERO_CHECK(on_block(std::move(*parsed)));
          }
          else if (boost::string_ref{*json}.starts_with(full_txpool_topic))
          {
            json->erase(0, sizeof(full_txpool_topic));
            auto parsed = rpc::full_txpool_pub::from_json(std::move(*json));
            if (!parsed)
            {
              MERROR("Failed parsing txpool pub: " << parsed.error().message());
              return parsed.error();
            }
            if (on_txpool)
              MONERO_CHECK(on_txpool(std::move(*parsed)));
          }
          else
            return {lws::error::bad_daemon_response};
        }
      }
    }

    return {};
  }

  expect<net::zmq::async_client> client::make_async_client(boost::asio::io_context& io) const
  {
    MONERO_PRECOND(ctx != nullptr);

    expect<net::zmq::socket> daemon = make_daemon(ctx);
    if (!daemon)
      return daemon.error();
    return net::zmq::async_client::make(io, std::move(*daemon));
  }

  expect<void> client::send(epee::byte_slice message, std::chrono::seconds timeout) noexcept
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(daemon != nullptr);
    assert(signal_sub != nullptr);

    expect<void> sent;
    while (!(sent = net::zmq::send(message.clone(), daemon.get(), ZMQ_DONTWAIT)))
    {
      if (sent != net::zmq::make_error_code(EAGAIN))
        return sent.error();

      MONERO_CHECK(do_wait(daemon.get(), signal_sub.get(), ZMQ_POLLOUT, timeout));
      timeout = std::chrono::seconds{0};
    }
    return success();
  }

  expect<void> client::publish(epee::byte_slice payload) const
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(daemon != nullptr);
    if (ctx->external_pub == nullptr && !ctx->rmq.is_available())
      return success();

    expect<void> rc = success();
    const boost::unique_lock<boost::mutex> guard{ctx->sync_pub};
    if (ctx->external_pub)
      rc = net::zmq::send(payload.clone(), ctx->external_pub.get(), 0);

#ifdef MLWS_RMQ_ENABLED

    if (ctx->rmq.is_available() && boost::algorithm::starts_with(payload, boost::string_ref{payment_topic_json()}))
    {
      const auto topic = reinterpret_cast<const std::uint8_t*>(std::memchr(payload.data(), ':', payload.size()));
      if (topic && topic >= payload.data())
        payload.remove_prefix(topic - payload.data() + 1);

      amqp_bytes_t message{};
      message.len = payload.size();
      message.bytes = const_cast<std::uint8_t*>(payload.data());

      int rmq_rc = 0;
      unsigned tries = 0;
      for (; tries < 2; ++tries)
      {
        rmq_rc = amqp_basic_publish(ctx->rmq.conn.get(), rmq_channel, amqp_cstring_bytes(ctx->rmq.info.exchange.c_str()), amqp_cstring_bytes(ctx->rmq.info.routing.c_str()), 0, 0, nullptr,  message);
        if (rmq_rc == 0)
          break;

        if (tries == 0)
        {
          MWARNING("Failed RMQ Publish with return code: " << rmq_rc << ". Retrying.");
          if (!ctx->rmq.connect())
            break;
        }
        else
          MERROR("Failed RMQ Publish with return code: " << rmq_rc << ". Dropping.");
      }

      if (rmq_rc)
        return {error::rmq_failure};
    }
#endif
    return rc;
  }

  expect<rates> client::get_rates() const
  {
    MONERO_PRECOND(ctx != nullptr);
    if (ctx->cache_interval <= std::chrono::minutes{0})
      return {lws::error::exchange_rates_disabled};

    const auto now  = std::chrono::steady_clock::now();
    const boost::unique_lock<boost::mutex> lock{ctx->sync_rates};
    if (now - ctx->cache_time >= ctx->cache_interval + std::chrono::seconds{30})
      return {lws::error::exchange_rates_old};
    return ctx->cached;
  }

  context context::make(std::string daemon_addr, std::string sub_addr, std::string pub_addr, rmq_details rmq_info, std::chrono::minutes rates_interval, const bool untrusted_daemon)
  {
    zcontext comm{zmq_init(1)};
    if (comm == nullptr)
      MONERO_THROW(net::zmq::get_error_code(), "zmq_init");

    net::zmq::socket pub{zmq_socket(comm.get(), ZMQ_PUB)};
    if (pub == nullptr)
      MONERO_THROW(net::zmq::get_error_code(), "zmq_socket");
    if (zmq_bind(pub.get(), signal_endpoint) < 0)
      MONERO_THROW(net::zmq::get_error_code(), "zmq_bind");

    net::zmq::socket external_pub = nullptr;
    if (!pub_addr.empty())
    {
      external_pub = net::zmq::socket{zmq_socket(comm.get(), ZMQ_PUB)};
      if (external_pub == nullptr)
        MONERO_THROW(net::zmq::get_error_code(), "zmq_socket");
      if (zmq_bind(external_pub.get(), pub_addr.c_str()) < 0)
        MONERO_THROW(net::zmq::get_error_code(), "zmq_bind");
    }

#ifndef MLWS_RMQ_ENABLED
    if (!rmq_info.address.empty() || !rmq_info.exchange.empty() || !rmq_info.routing.empty() || !rmq_info.credentials.empty())
      MONERO_THROW(error::configuration, "RabbitMQ support not enabled");
#endif
 
    return context{
      std::make_shared<detail::context>(
        std::move(comm), std::move(pub), std::move(external_pub), rcontext{std::move(rmq_info)}, std::move(daemon_addr), std::move(sub_addr), rates_interval, untrusted_daemon
      )
    };
  }

  context::~context() noexcept
  {
    if (ctx)
      raise_abort_process();
  }

  void* context::zmq_context() const
  {
    if (ctx == nullptr)
      return nullptr;
    return ctx->comm.get();
  }

  std::string const& context::daemon_address() const
  {
    if (ctx == nullptr)
      MONERO_THROW(common_error::kInvalidArgument, "Invalid lws::rpc::context");
    return ctx->daemon_addr;
  }

  std::string const& context::pub_address() const
  {
    if (ctx == nullptr)
      MONERO_THROW(common_error::kInvalidArgument, "Invalid lws::rpc::context");
    return ctx->sub_addr;
  }

  std::chrono::minutes context::cache_interval() const
  {
    if (ctx == nullptr)
      MONERO_THROW(common_error::kInvalidArgument, "Invalid lws::rpc::context");
    return ctx->cache_interval;
  }

  expect<void> context::raise_abort_scan() noexcept
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(ctx->signal_pub != nullptr);
    return do_signal(ctx->signal_pub.get(), abort_scan_signal);
  }

  expect<void> context::raise_abort_process() noexcept
  {
    MONERO_PRECOND(ctx != nullptr);
    assert(ctx->signal_pub != nullptr);
    return do_signal(ctx->signal_pub.get(), abort_process_signal);
  }

  expect<void> context::retrieve_rates_async(boost::asio::io_context& io)
  {
    MONERO_PRECOND(ctx != nullptr);

    if (ctx->cache_interval <= std::chrono::minutes{0})
      return success();

    if (ctx->rates_running.test_and_set())
      return success();

    auto& self = ctx;
    const expect<void> rc = ctx->rates_conn.get_async(
      io, crypto_compare.url, [self] (boost::system::error_code error, std::string body)
      {
        expect<rates> fresh{lws::error::exchange_rates_fetch};
        if (!error)
        {
          fresh = crypto_compare(std::move(body));
          if (fresh)
            MINFO("Updated exchange rates: " << *fresh);
          else
            MERROR("Failed to parse exchange rates: " << fresh.error());
        }
        else
          MERROR("Failed to retrieve exchange rates: " << error.message());

        const auto now = std::chrono::steady_clock::now();
        if (fresh)
        {
          const boost::lock_guard<boost::mutex> lock{self->sync_rates};
          self->cache_time = now;
          self->cached = std::move(*fresh);
        }
        self->rates_running.clear();
      });
    if (!rc)
      ctx->rates_running.clear();
    return rc;
  }
} // rpc
} // lws
