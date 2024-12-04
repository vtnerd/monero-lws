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

#include "client.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/optional/optional.hpp>
#include <boost/thread/lock_types.hpp>
#include <cstdint>
#include <deque>
#include <limits>
#include <ostream>

#include "error.h"
#include "misc_log_ex.h"           // monero/contrib/epee/include
#include "net/http_base.h"         // monero/contrib/epee/include
#include "net/http/slice_body.h"
#include "net/net_parse_helpers.h" // monero/contrib/epee/include

namespace net { namespace http
{
  namespace
  {
    constexpr const unsigned http_version = 11;
    constexpr const std::size_t http_parser_buffer_size = 16 * 1024;
    constexpr const std::size_t max_body_size = 4 * 1024;

    //! Timeout for 1 entire HTTP request (connect, handshake, send, receive).
    constexpr const std::chrono::seconds message_timeout{30};

    struct message
    {
      message(epee::byte_slice json_body, std::string host, std::string target, std::uint16_t port, bool https, std::function<server_response_func>&& notifier)
        : json_body(std::move(json_body)),
          notifier(std::move(notifier)),
          host(std::move(host)),
          target(std::move(target)),
          port(port),
          https(https)
      {}

      message(message&&) = default;
      message(const message& rhs)
        : json_body(rhs.json_body.clone()),
          notifier(rhs.notifier),
          host(rhs.host),
          target(rhs.target),
          port(rhs.port),
          https(rhs.https)
      {}

      epee::byte_slice json_body;
      std::function<server_response_func> notifier;
      std::string host;
      std::string target;
      std::uint16_t port;
      bool https;
    };

    std::ostream& operator<<(std::ostream& out, const message& src)
    {
      out << (src.https ? "https://" : "http://") << src.host << ':' << src.port << src.target;
      return out;
    }
  } // anonymous

  struct client_state
  {
    std::shared_ptr<boost::asio::ssl::context> ssl;
    boost::beast::flat_static_buffer<http_parser_buffer_size> buffer;
    std::deque<message> outgoing;
    std::string last_host;
    boost::asio::ip::tcp::resolver resolver;
    boost::asio::steady_timer timer;
    boost::asio::io_context::strand strand;
    boost::asio::ip::tcp::endpoint endpoint;
    boost::beast::http::request<slice_body> request;
    boost::optional<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> sock;
    boost::optional<boost::beast::http::parser<false, boost::beast::http::string_body>> parser;
    std::size_t iteration;

    client_state(boost::asio::io_context& io, std::shared_ptr<boost::asio::ssl::context> in)
      : ssl(std::move(in)),
        buffer{},
        outgoing(),
        last_host(),
        resolver(io),
        timer(io),
        strand(io),
        endpoint(),
        request{},
        sock(),
        parser(),
        iteration(0)
    {
      assert(ssl);
      sock.emplace(io, *ssl);
    }

    template<typename F>
    void async_write(F&& callback)
    {
      assert(sock);
      assert(!outgoing.empty());
      const bool no_body = outgoing.front().json_body.empty();
      request = {
        outgoing.front().notifier ? boost::beast::http::verb::get : boost::beast::http::verb::post,
        outgoing.front().target,
        http_version,
        std::move(outgoing.front().json_body)
      };
      request.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
      if (!no_body)
        request.set(boost::beast::http::field::content_type, "application/json");

      // Setting Host is tricky. Check for v6 and non-standard ports
      boost::system::error_code error{};
      boost::asio::ip::make_address_v6(outgoing.front().host, error);
      if (!error)
        request.set(boost::beast::http::field::host, "[" + outgoing.front().host + "]:" + std::to_string(outgoing.front().port));
      else if ((outgoing.front().https && outgoing.front().port == 443) || (!outgoing.front().https && outgoing.front().port == 80))
        request.set(boost::beast::http::field::host, outgoing.front().host);
      else
        request.set(boost::beast::http::field::host, outgoing.front().host + ":" + std::to_string(outgoing.front().port));

      request.prepare_payload();
      if (outgoing.front().https)
        boost::beast::http::async_write(*sock, request, boost::asio::bind_executor(strand, std::forward<F>(callback)));
      else
        boost::beast::http::async_write(sock->next_layer(), request, boost::asio::bind_executor(strand, std::forward<F>(callback)));
    }

    template<typename F>
    void async_read(F&& callback)
    {
      assert(sock);
      assert(!outgoing.empty());
      parser.emplace();
      parser->body_limit(max_body_size);
      if (outgoing.front().https)
        boost::beast::http::async_read(*sock, buffer, *parser, boost::asio::bind_executor(strand, std::forward<F>(callback)));
      else
        boost::beast::http::async_read(sock->next_layer(), buffer, *parser, boost::asio::bind_executor(strand, std::forward<F>(callback)));
    }

    void notify_error(const boost::system::error_code& error) const
    {
      assert(!outgoing.empty()); 
      if (outgoing.front().notifier)
        outgoing.front().notifier(error, {});
    }
  };  

  namespace
  {
    class client_loop : public boost::asio::coroutine
    {
      std::shared_ptr<client_state> self_;

    public:
      explicit client_loop(std::shared_ptr<client_state> self) noexcept
        : boost::asio::coroutine(), self_(std::move(self))
      {}

      bool set_timeout(const std::chrono::steady_clock::duration timeout)
      {
        if (!self_)
          return false;

        struct on_timeout
        {
          on_timeout() = delete;
          std::shared_ptr<client_state> self_;
          std::size_t iteration;

          void operator()(boost::system::error_code error) const
          {
            if (!self_ || error == boost::asio::error::operation_aborted)
              return;
            if (iteration < self_->iteration)
              return;

            assert(self_->strand.running_in_this_thread());
            assert(self_->sock);
            if (!self_->outgoing.empty())
              MWARNING("Timeout in HTTP attempt to " << self_->outgoing.front());
            else
              MERROR("Unexpected empty message stack");

            self_->resolver.cancel();
            self_->sock->next_layer().cancel(error);
            self_->sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
          }
        };

        self_->timer.expires_after(timeout);
        self_->timer.async_wait(boost::asio::bind_executor(self_->strand, on_timeout{self_, self_->iteration}));
        return true;
      }

      void operator()(boost::system::error_code error = {}, std::size_t = 0)
      {
        if (!self_)
          return;

        bool reuse = false;
        bool is_https = false;
        std::uint16_t last_port = 0;
        client_state& self = *self_;
        assert(self.strand.running_in_this_thread());
        assert(self.sock);
        BOOST_ASIO_CORO_REENTER(*this)
        {
          while (!self.outgoing.empty())
          {
            struct resolve
            {
              client_loop continue_;

              void operator()(const boost::system::error_code error, const boost::asio::ip::tcp::resolver::results_type& ips)
              {
                if (error)
                  std::move(continue_)(error, 0);
                else if (ips.empty())
                  std::move(continue_)(boost::asio::error::host_not_found, 0);
                else if (continue_.self_)
                {
                  continue_.self_->endpoint = *ips.begin();
                  std::move(continue_)(error, 0);
                }
              }
            };

            set_timeout(message_timeout);

            if (!reuse)
            {
              MDEBUG("Resolving " << self.outgoing.front().host << " for HTTP");
              BOOST_ASIO_CORO_YIELD self.resolver.async_resolve(
                self.outgoing.front().host,
                std::to_string(self.outgoing.front().port),
                boost::asio::bind_executor(self.strand, resolve{std::move(*this)})
              );
            }

            if (!error)
            {
              if (!reuse)
              {
                MDEBUG("Connecting to " << self.endpoint << " / " << self.outgoing.front().host << " for HTTP");
                BOOST_ASIO_CORO_YIELD self.sock->next_layer().async_connect(
                  self.endpoint, boost::asio::bind_executor(self.strand, std::move(*this))
                );
                self.buffer.clear(); // do not re-use http buffer
              }

              if (!error)
              {
                if (!reuse && self.outgoing.front().https)
                {
                  {
                    SSL* const ssl_ctx = self.sock->native_handle();                                
                    if (ssl_ctx)
                      SSL_set_tlsext_host_name(ssl_ctx, self.outgoing.front().host.c_str());
                  }
                  MDEBUG("Starting SSL handshake to " << self.outgoing.front().host << " for HTTP");
                  BOOST_ASIO_CORO_YIELD self.sock->async_handshake(
                    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>::client,
                    boost::asio::bind_executor(self.strand, std::move(*this))
                  );
                }

                if (!error)
                {
                  reuse = false;
                  MDEBUG("Sending " << self.outgoing.front().json_body.size() << " bytes in HTTP " << (self.outgoing.front().notifier ? "GET" : "POST") << " to " << self.outgoing.front());
                  BOOST_ASIO_CORO_YIELD self.async_write(std::move(*this));
                  
                  if (!error)
                  {
                    MDEBUG("Starting read from " << self.outgoing.front() << " to previous HTTP message");
                    BOOST_ASIO_CORO_YIELD self.async_read(std::move(*this));
                    
                    if (error)
                      MERROR("Failed to parse HTTP response from " << self.outgoing.front() << ": " << error.message());
                    else if (self.parser->get().result_int() != 200 && self.parser->get().result_int() != 201)
                    {
                      MERROR(self.outgoing.front() << " returned " << self.parser->get().result_int() << " status code");
                      self.notify_error(boost::asio::error::operation_not_supported);
                    }
                    else
                    {
                      MDEBUG(self.outgoing.front() << " successful");
                      reuse = self.parser->get().keep_alive();
                      if (self.outgoing.front().notifier)
                        self.outgoing.front().notifier({}, std::move(self.parser->get()).body());
                    }
                  }
                  else
                    MERROR("Failed HTTP " << (self.outgoing.front().notifier ? "GET" : "POST") << " to " << self.outgoing.front() << ": " << error.message());
                }
                else
                  MERROR("SSL handshake to " << self.outgoing.front().host << " failed: " << error.message());
              }
              else
                MERROR("Failed to connect to " << self.outgoing.front().host << ": " << error.message());
            }
            else
              MERROR("Failed to resolve TCP/IP address for " << self.outgoing.front().host << ": " << error.message());

            if (error)
              self.notify_error(error);

            is_https = self.outgoing.front().https;
            self.last_host = std::move(self.outgoing.front().host);
            last_port = self.outgoing.front().port;
            self.outgoing.pop_front();

            reuse = reuse &&
              !self.outgoing.empty() && 
              self.last_host == self.outgoing.front().host &&
              last_port == self.outgoing.front().port;

            if (!reuse)
            {
              if (is_https)
              {
                MDEBUG("Starting SSL shutdown on " << self.last_host);
                BOOST_ASIO_CORO_YIELD self.sock->async_shutdown(
                  boost::asio::bind_executor(self.strand, std::move(*this))
                );
                is_https = true;
              }

              MDEBUG("Cleaning up connection to " << self.last_host);
              self.sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
              self.sock->next_layer().close(error);
              if (is_https) // must clear SSL state
                self.sock.emplace(self.timer.get_executor(), *self.ssl);
            }
 
            ++self.iteration;
          }
        }
      }
    };

    boost::asio::ssl::context make_context(epee::net_utils::ssl_verification_t verify)
    {
      epee::net_utils::ssl_options_t ssl{
        epee::net_utils::ssl_support_t::e_ssl_support_enabled
      };
      ssl.verification = verify;
      return ssl.create_context();
    }
  } // anonymous

  expect<void> client::queue_async(boost::asio::io_context& io, std::string url, epee::byte_slice json_body, std::function<server_response_func> notifier)
  {
    static constexpr const std::uint16_t max_port = std::numeric_limits<std::uint16_t>::max();

    epee::net_utils::http::url_content parsed{};
    if (!epee::net_utils::parse_url(std::move(url), parsed) || max_port < parsed.port)
      return {lws::error::bad_url};
    if (parsed.schema != "http" && parsed.schema != "https")
      return {lws::error::bad_url};

    if (!parsed.port)
      parsed.port = (parsed.schema == "http") ? 80 : 443;

    if (parsed.uri.empty())
      parsed.uri = "/";

    message msg{
      std::move(json_body),
      std::move(parsed.host),
      std::move(parsed.uri),
      std::uint16_t(parsed.port),
      bool(parsed.schema == "https"),
      std::move(notifier)
    };

    MDEBUG("Queueing HTTP " << (msg.notifier ? "GET" : "POST") << " to " << msg << " using " << this);
    boost::unique_lock<boost::mutex> lock{sync_};
    auto state = state_.lock();
    if (!state)
    {
      // `make_shared` delays freeing of data section, use `make_unique`
      MDEBUG("Creating new net::http::client_state for " << this);
      state = {std::make_unique<client_state>(io, ssl_)};
      state_ = state;
      state->outgoing.push_back(std::move(msg));

      lock.unlock();
      boost::asio::post(state->strand, client_loop{state});
    }
    else
    {
      lock.unlock();
      boost::asio::dispatch(
        state->strand,
        [state, msg = std::move(msg)] () mutable
        {
          const bool empty = state->outgoing.empty();
          state->outgoing.push_back(std::move(msg));
          if (empty)
            boost::asio::post(state->strand, client_loop{state});
        }
      );
    }

    return success();
  }

  client::client(epee::net_utils::ssl_verification_t verify)
    : state_(),
      ssl_(std::make_shared<boost::asio::ssl::context>(make_context(verify))),
      sync_()
  {}

  client::client(std::shared_ptr<boost::asio::ssl::context> ssl)
    : state_(), ssl_(std::move(ssl)), sync_()
  {
    if (!ssl_)
      throw std::logic_error{"boost::asio::ssl::context cannot be nullptr"};
  }

  client::~client()
  {}

  expect<void> client::post_async(boost::asio::io_context& io, std::string url, epee::byte_slice json_body)
  {
    return queue_async(io, std::move(url), std::move(json_body), {});
  }

  expect<void> client::get_async(boost::asio::io_context& io, std::string url, std::function<server_response_func> notifier)
  {
    if (!notifier)
      throw std::logic_error{"net::http::client::get_async requires callback"};
    return queue_async(io, std::move(url), epee::byte_slice{}, std::move(notifier));
  }
}} // net // http

