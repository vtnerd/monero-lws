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

/* This file was split into two compilation units `feed_tcp.cpp` and
  `feed_ssl.cpp`. This helps reduce the compilation time since each file can be
  computed in parallel. The memory usage is also reduced when -j1, which helps
  with compilation on smaller machines.

  The functions must be in anonymous namespace or  marked inline (implicitly or
  explicitly) or linker will have issues. Inline is preferred because the
  linker will merge the duplicate copies. */

#include "feed.h"

#include <array>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/string_type.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <chrono>
#include <type_traits>
#include <utility>

#include "db/storage.h"
#include "error.h"
#include "misc_log_ex.h"
#include "net/http/slice_body.h"
#include "net/zmq_async.h"
#include "rpc/client.h"

namespace lws { namespace rpc { namespace feed
{
  constexpr const std::size_t max_read_size = 2 * 1024; // login call is small
  constexpr const std::size_t queue_max = 100; // # messages in queue

  //! Suppress scanner messages in this interval when account is catching up
  constexpr const std::chrono::seconds scanner_busy{10};

  //! Send all block updates if account is within this top blockchain height
  constexpr const unsigned on_tip = 10;

  constexpr const std::chrono::seconds close_timeout{8}; // for websocket close
  constexpr const std::chrono::seconds handshake_timeout{5};
  constexpr const std::chrono::seconds login_timeout{8};
  constexpr const std::chrono::seconds shutdown_timeout{8}; // for ssl shutdown
  constexpr const std::chrono::seconds tx_sync_timeout{20}; // for initial tx sync

  enum class event { none = 0, invalid, shutdown, timeout};
  enum class protocol : unsigned { invalid = 0, v0_json, v0_msgpack };

  inline constexpr const char* get_string(const protocol proto) noexcept
  {
    switch (proto)
    {
    case protocol::v0_json:
      return "lws.feed.v0.json";
    case protocol::v0_msgpack:
      return "lws.feed.v0.msgpack";
    default:
      break;
    }

    return "unsupported";
  }

  // defined in `feed_tcp.cpp. Selects preferred protocol
  protocol get_protocol(const boost::beast::string_view list) noexcept;

  inline bool is_binary(const protocol proto) noexcept
  {
    switch (proto)
    {
    case protocol::v0_msgpack:
      return true;
    default:
      break;
    }
    return false;
  }
}}} // lws // rpc // feed

namespace boost
{
  namespace system
  {
    template <>
    struct is_error_code_enum<lws::rpc::feed::event>
      : std::true_type
    {};
  }  // namespace system
}  // namespace boost

namespace lws { namespace rpc { namespace feed
{
  
  // defined in `feed_tcp.cpp
  const boost::system::error_category& get_event_category();
  inline boost::system::error_code make_error_code(event e)
  {
    return {static_cast<int>(e), get_event_category()};
  }

  class connection;

  struct connection_sync
  {
    db::block_id height;
    db::block_id last_reported;
    std::chrono::steady_clock::time_point last_update;
    db::account_id id;
    std::uint32_t warnings;
    bool on_tip;
    bool receives_only;

    connection_sync() noexcept
      : height(db::block_id::txpool),
        last_reported(db::block_id::txpool),
        last_update(),
        id(db::account_id::invalid),
        warnings(-1),
        on_tip(false),
        receives_only(false)
    {}
  };

  //! Handles initial login, tx sync, and then waits on zmq messages, pushing them to a queue
  struct zmq_loop : boost::asio::coroutine
  {
    std::shared_ptr<connection> self_;
    std::size_t iteration_;

    explicit zmq_loop(std::shared_ptr<connection> self)
      : boost::asio::coroutine(), self_(std::move(self)), iteration_(0)
    {}

    void operator()(boost::system::error_code error = {}, std::size_t = {});
  };
 
  // defined in `feed_tcp.cpp`
  std::string_view get_prefix(const boost::beast::net::const_buffer buf);
  expect<epee::byte_slice> prep_error(std::error_code error, protocol proto);
  expect<epee::byte_slice> prep_login(const db::storage& disk, account_sub* sub, connection_sync* sync, boost::beast::flat_buffer& buffer, protocol proto);
  expect<epee::byte_slice> prep_update(const db::storage& disk, std::string&& source, connection_sync* sync, protocol proto);

  class connection
  {
    lws::db::storage disk_;
    std::string sub_buffer_;
    boost::beast::flat_buffer ws_buffer_;
    std::deque<epee::byte_slice> write_queue_;
    account_sub sub_;
    boost::asio::io_context::strand strand_;
    boost::asio::steady_timer socket_timer_;
    std::size_t pull_iter_;
    connection_sync sync_;
    const protocol proto_;

    virtual void async_shutdown(std::shared_ptr<connection> self) = 0;

  protected:

    template<typename T>
    void do_async_close(T& sock, std::shared_ptr<connection> self)
    {
      LWS_VERIFY(self);
      MDEBUG("Starting websocket close on " << this);
      boost::system::error_code ec{};
      sub_.shutdown(ec);
      start_timeout(close_timeout, self);
      sock.async_close(boost::beast::websocket::close_reason(), bind(std::bind(&connection::async_shutdown, self, self)));
    }

    void do_async_shutdown(boost::asio::ip::tcp::socket& sock, std::shared_ptr<connection>)
    {
      do_cleanup(sock);
    }

    void do_async_shutdown(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& sock, std::shared_ptr<connection> self)
    {
      LWS_VERIFY(self);
      MDEBUG("Starting TLS/SSL shutdown on " << this);
      boost::system::error_code ec{};
      sub_.shutdown(ec);
      start_timeout(shutdown_timeout, self);
      sock.async_shutdown(bind(std::bind(&connection::cleanup, self)));
    }

    void do_cleanup(boost::asio::ip::tcp::socket& sock)
    {
      MDEBUG("Websocket cleanup on " << this);
      boost::system::error_code ec{};
      sub_.shutdown(ec);
      socket_timer_.cancel();
      sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      sock.close(ec);
    }

    void do_cleanup(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& sock)
    {
      do_cleanup(sock.next_layer());
    }

    template<typename T>
    void do_stop_reads(T& sock, std::shared_ptr<connection> self)
    {
      LWS_VERIFY(self);
      ws_buffer_.shrink_to_fit();
      sock.async_read(ws_buffer_, bind(std::bind(&connection::close_check, self, self, event::invalid)));
    }

    template<typename T>
    void do_async_read(T& sock, boost::asio::steady_timer::duration timeout, zmq_loop callback)
    {
      if (is_closing())
        return callback(event::shutdown);

      LWS_VERIFY(callback.self_);
      start_timeout(timeout, callback);
      sock.async_read(ws_buffer_, bind(std::move(callback)));
    }

    template<typename T>
    static void do_async_write_queue(T& sock, std::shared_ptr<connection> self, epee::byte_slice&& first)
    {
      // do not check `is_closing()`. Instead, try to write out all buffered data
      LWS_VERIFY(self && self->write_queue_.empty());

      struct push_loop
      {
        T* sock_;
        std::shared_ptr<connection> self_;

        void async_write()
        {
          LWS_VERIFY(sock_ && self_ && !self_->write_queue_.empty());
          connection& self = *self_;

          const epee::byte_slice& next = self.write_queue_.front();
          MDEBUG("Sending websocket " << get_prefix({next.data(), next.size()}) << " message on " << self_.get());
          sock_->async_write(
            boost::asio::buffer(next.data(), next.size()), self.bind(std::move(*this))
          );
        }

        void operator()(const boost::system::error_code error, std::size_t)
        {
          LWS_VERIFY(self_ && !self_->write_queue_.empty());
          connection& self = *self_;

          if (error)
          {
            MDEBUG("Write error on websocket (" << self_.get() << "): " << error.message());
            return self.async_close(std::move(self_));
          }

          self.write_queue_.pop_front();
          if (!self.write_queue_.empty())
            async_write();
          else if (self.is_closing())
            return self.async_close(std::move(self_));
        }
      };
 
      self->write_queue_.push_back(std::move(first));
      push_loop{std::addressof(sock), std::move(self)}.async_write();
    }

    bool do_write(std::shared_ptr<connection> self, expect<epee::byte_slice>&& message)
    {
      const bool rc = message.has_value();
      if (!rc)
      {
        MDEBUG("Error in websocket (" << this << "): " << message.error().message());
        message = MONERO_UNWRAP(prep_error(message.error(), proto_));
      }
      else if (message->empty())
        return true; // nop update (scanner is _very_ active)

      if (write_queue_.empty())
        async_write_queue(std::move(self), std::move(*message));
      else
        write_queue_.push_back(std::move(*message));
      return rc;
    }

    static bool sync_iter(std::size_t& ours, const std::size_t theirs) noexcept
    {
      if (ours != theirs)
        return false;
      ++ours;
      return true;
    }

   public: 
    connection(boost::asio::io_context& io, const lws::rpc::client& client, const lws::db::storage& disk, const protocol proto)
      : disk_(disk.clone()),
        sub_buffer_(),
        ws_buffer_(),
        write_queue_(),
        sub_(MONERO_UNWRAP(client.make_account_sub(io))),
        strand_(io),
        socket_timer_(io),
        pull_iter_(0),
        sync_{},
        proto_(proto)
    {
      ws_buffer_.max_size(max_read_size);
    }
        
    virtual ~connection() noexcept { MDEBUG("Destroying websocket on " << this); }

    protocol proto() const noexcept { return proto_; }
    bool is_closing() const noexcept { return !bool(sub_); }
    bool sync_pull(const std::size_t iter) noexcept { return sync_iter(pull_iter_, iter); }

    bool login(std::shared_ptr<connection> self)
    {
      const bool rc = do_write(std::move(self), prep_login(disk_, &sub_, &sync_, ws_buffer_, proto_));
      ws_buffer_.consume(ws_buffer_.cdata().size());
      return rc;
    }

    // Push message from ZMQ to websocket (maybe)
    bool push(std::shared_ptr<connection> self)
    {
      bool rc = false;
      if (queue_max < write_queue_.size())
      {
        MDEBUG("Websocket max queue reached on " << this);
        write_queue_.push_back(MONERO_UNWRAP(prep_error({error::queue_max}, proto_)));
      }
      else
      {
        MDEBUG("Websocket queued event from scanner on " << this);
        rc = do_write(std::move(self), prep_update(disk_,  std::move(sub_buffer_), &sync_, proto_));
      }
      return rc;
    }

    template<typename F>
    auto bind(F&& f) -> decltype(boost::asio::bind_executor(strand_, std::forward<F>(f)))
    {
      return boost::asio::bind_executor(strand_, std::forward<F>(f));
    }

    void client_close(const boost::system::error_code error = {})
    {
      if (error)
        MDEBUG("Preparing websocket close (" << this << "): " << error.message());
      else
        MDEBUG("Preparing websocket close on " << this);
      boost::system::error_code ec{};
      socket_timer_.cancel();
      sub_.shutdown(ec);
      // writes may need to be flushed, so don't cancel socket ops
    }

    void close_check(std::shared_ptr<connection> self, const boost::system::error_code error = {})
    {
      if (write_queue_.empty())
        async_close(std::move(self));
      else
        client_close(error);
    }

    void start_timeout(boost::asio::steady_timer::duration timeout, std::shared_ptr<connection> self)
    {
      struct force_close
      {
        std::shared_ptr<connection> self_;

        void operator()(const boost::system::error_code error) const
        {
          if (error != boost::asio::error::operation_aborted)
          {
            LWS_VERIFY(self_);
            MDEBUG("Websocket timeout on " << self_.get());
            self_->cleanup();
          }
        }
      };

      LWS_VERIFY(self);
      socket_timer_.expires_after(timeout);
      socket_timer_.async_wait(bind(force_close{std::move(self)}));
    }

    void start_timeout(boost::asio::steady_timer::duration timeout, zmq_loop handler)
    {
      struct notify
      {
        zmq_loop f_;

        void operator()(const boost::system::error_code error)
        {
          if (!error)
            f_(event::timeout);
          else if (error != boost::asio::error::operation_aborted)
            f_(error);
          // else another timer was queued via expires_after
        }
      };

      LWS_VERIFY(handler.self_);
      socket_timer_.expires_after(timeout);
      socket_timer_.async_wait(bind(notify{std::move(handler)}));
    }
 
    void async_zmq_wait(zmq_loop callback)
    {
      if (is_closing())
        return callback(event::shutdown);

      // Skip timeout
      LWS_VERIFY(callback.self_);
      net::zmq::async_read(sub_.get(), sub_buffer_, bind(std::move(callback)));
    }

    //! Start socket close routine. `loop::` must be exited after this.
    virtual void async_close(std::shared_ptr<connection> self) = 0; 
    virtual void cleanup() = 0;
    virtual void stop_reads(std::shared_ptr<connection> self) = 0;
    virtual void async_read(boost::asio::steady_timer::duration timeout, zmq_loop callback) = 0;
    virtual void async_write_queue(std::shared_ptr<connection> self, epee::byte_slice&& msg) = 0;
  };

  template<typename T>
  class connection_ final : public connection
  {
    boost::beast::websocket::stream<T, false> sock_;

    virtual void async_shutdown(std::shared_ptr<connection> self) override final
    { do_async_shutdown(sock().next_layer(), std::move(self)); }

  public:
    explicit connection_(T&& sock, boost::asio::io_context& io, const lws::rpc::client& client, const lws::db::storage& disk, const protocol proto)
      : connection(io, client, disk, proto), sock_(std::move(sock))
    {
      if (is_binary(proto))
        sock_.binary(true);
    }

    boost::beast::websocket::stream<T, false>& sock() { return sock_; }

    virtual void async_close(std::shared_ptr<connection> self) override final
    { do_async_close(sock(), std::move(self)); }

    virtual void cleanup() override final { do_cleanup(sock().next_layer()); }

    virtual void stop_reads(std::shared_ptr<connection> self) override final
    { do_stop_reads(sock(), std::move(self)); }

    virtual void async_read(boost::asio::steady_timer::duration timeout, zmq_loop callback) override final
    { do_async_read(sock(), timeout, std::move(callback)); }

    virtual void async_write_queue(std::shared_ptr<connection> self, epee::byte_slice&& msg) override final
    { do_async_write_queue(sock(), std::move(self), std::move(msg)); } 
  };

 
  inline void zmq_loop::operator()(const boost::system::error_code error, const std::size_t bytes)
  {
    LWS_VERIFY(self_);
    connection& self = *self_;
    if (!self.sync_pull(iteration_)) // sync all copies of loop
      return;
    ++iteration_;
 
    BOOST_ASIO_CORO_REENTER(*this)
    {
      if (!error) // handshake
      {
        BOOST_ASIO_CORO_YIELD self.async_read(login_timeout, std::move(*this));
        if (!error)
        {
          self.stop_reads(self_); // client can only send control frames now
          if (!self.login(self_)) // always queues a write
            return self.client_close();
        }
      }

      if (error)
      {
        MDEBUG("Websocket failure (" << self_.get() << "): " << error.message());
        return self.async_close(std::move(self_)); // login never queued write
      }

      while (true)
      {
        BOOST_ASIO_CORO_YIELD self.async_zmq_wait(std::move(*this));
        if (error || !self.push(self_))
          return self.close_check(self_, error);
      }
    }
  }  
 
  inline const boost::asio::ip::tcp::socket& get_tcp_sock(const boost::asio::ip::tcp::socket& sock) noexcept { return sock; }
  inline const boost::asio::ip::tcp::socket& get_tcp_sock(const boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& sock) { return sock.next_layer(); } 

  template<typename T>
  inline void do_start(std::shared_ptr<connection_<T>> self, const request& req, const std::chrono::seconds timeout)
  {
    struct on_control
    {
      std::weak_ptr<connection> self_;
      void operator()(boost::beast::websocket::frame_type type, boost::beast::string_view)
      {
        switch (type)
        {
        default:
        case boost::beast::websocket::frame_type::close:
        {
          const auto self = self_.lock();
          if (!self)
            return;
          MDEBUG("Websocket client requested close on " << self.get());
          self->close_check(self);
          break;
        }
        case boost::beast::websocket::frame_type::ping:
        case boost::beast::websocket::frame_type::pong:
          MDEBUG("Received control frame (ping/pong) from " << self_.lock().get());
          break;
        }
      }
    };

    LWS_VERIFY(self);
    connection_<T>& obj = *self;

    boost::system::error_code ec{};
    MDEBUG("Starting websocket handshake on " << get_tcp_sock(self->sock().next_layer()).remote_endpoint(ec) << " / " << self.get());
    obj.sock().control_callback(on_control{self});
    {
      namespace http = boost::beast::http;
      using stream = boost::beast::websocket::stream_base;
      obj.sock().set_option(stream::timeout{handshake_timeout, timeout, true /* pings */});

      const protocol proto = obj.proto();
      obj.sock().set_option(
        stream::decorator(
          [proto] (http::response_header<>& hdr)
          { hdr.set(http::field::sec_websocket_protocol, get_string(proto)); }
        )
      );
    }

    obj.sock().async_accept(req, obj.bind(zmq_loop{std::move(self)}));
  }

  template<typename T>
  inline bool do_start(T&& sock, boost::asio::io_context& io, const lws::rpc::client& client, const request& req, const lws::db::storage& disk, const std::chrono::seconds timeout)
  {
    const protocol proto = get_protocol(req.base()[boost::beast::http::field::sec_websocket_protocol]);
    switch (proto)
    {
    case protocol::v0_msgpack:
    case protocol::v0_json:
      do_start(std::make_shared<connection_<T>>(std::forward<T>(sock), io, client, disk, proto), req, timeout);
      return true;
    default:
    case protocol::invalid:
      break;
    }
    return false;
  }
}}} // lws // rpc // feed

