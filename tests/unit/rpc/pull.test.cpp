// Copyright (c) 2026 The Monero Project
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

#include "pull.test.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/string_type.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/stream.hpp>

#include "error.h"
#include "misc_log_ex.h"
namespace lws_test { namespace rpc { namespace pull
{
  struct connection
  {
    boost::asio::io_context::strand strand_;
    boost::beast::flat_buffer buffer_;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> stream_;
    const boost::asio::ip::tcp::endpoint remote_;
    std::string out_;

    explicit connection(boost::asio::io_context& io, const boost::asio::ip::tcp::endpoint& remote)
      : strand_(io), buffer_(), stream_(io), remote_(remote), out_()
    {}
  };

  namespace
  {
    struct wrapper
    {
      std::shared_ptr<connection> self_;
      std::function<response> f_;

      void operator()(const boost::system::error_code error, std::size_t = {}) const
      {
        LWS_VERIFY(self_ && f_);
        if (error)
          f_(error, {});

        const boost::beast::net::const_buffer buf = self_->buffer_.cdata();
        std::string msg{reinterpret_cast<const char*>(buf.data()), buf.size()};
        self_->buffer_.consume(buf.size());
        f_(error, std::move(msg));
      }
    };

    class connector : public boost::asio::coroutine
    {
      wrapper self_;

    public:
      explicit connector(wrapper&& self)
        : boost::asio::coroutine(), self_(std::move(self)) 
      {}

      connector(const connector&) = default;

      void operator()(const boost::system::error_code error = {}, std::size_t = {})
      {
        LWS_VERIFY(self_.self_);
        connection& self = *self_.self_;
        if (error)
          return self_(error);

        BOOST_ASIO_CORO_REENTER(*this)
        {
          BOOST_ASIO_CORO_YIELD self.stream_.next_layer().async_connect(self.remote_, *this);
          BOOST_ASIO_CORO_YIELD self.stream_.async_handshake("localhost", "/feed", *this);
          BOOST_ASIO_CORO_YIELD self.stream_.async_write(boost::asio::const_buffer(self.out_.data(), self.out_.size()), *this);
          BOOST_ASIO_CORO_YIELD self.stream_.async_read(self.buffer_, *this);
          self_(error);
        }
      }
    };
  } // anonymous

  std::shared_ptr<connection> make(boost::asio::io_context& io, const boost::asio::ip::tcp::endpoint& remote, const std::string& version)
  {
    namespace ws = boost::beast::websocket;
    namespace http = boost::beast::http;
    const auto ptr = std::make_shared<connection>(io, remote);
    ptr->stream_.set_option(
      ws::stream_base::decorator(
        [version] (http::request_header<>& hdr) { hdr.set(http::field::sec_websocket_protocol, version); }
      )
    );
    return ptr;
  }

  void async_handshake(std::shared_ptr<connection> self, std::string init_message, std::function<response> resp)
  {
    LWS_VERIFY(self);
    self->out_ = std::move(init_message);
    connector{wrapper{self, std::move(resp)}}();
  }

  void async_next_message(std::shared_ptr<connection> self, std::function<response> resp)
  {
    LWS_VERIFY(self);
    self->stream_.async_read(self->buffer_, wrapper{self, std::move(resp)});
  }

  void async_close(std::shared_ptr<connection> self, std::function<response> resp)
  {
    LWS_VERIFY(self);
    self->stream_.async_close(boost::beast::websocket::close_reason(), wrapper{self, std::move(resp)});
  }
}}} // lws_Test // rpc // pull
