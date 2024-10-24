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

#include <boost/asio/compose.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <zmq.h>
#include "byte_slice.h"     // monero/contrib/epee/include
#include "common/expect.h"  // monero/src
#include "net/zmq.h"

namespace net { namespace zmq
{
  //! \return Category for ZMQ errors.
  const boost::system::error_category& boost_error_category() noexcept;

  //! \return `code` iff the `::net::zmq` error category
  boost::system::error_code make_error_code(std::error_code code);

  using adescriptor = boost::asio::posix::stream_descriptor;

  struct free_descriptor
  {
    void operator()(adescriptor* ptr) noexcept;
  };

  using asocket = std::unique_ptr<adescriptor, free_descriptor>;

  struct async_client
  {
    async_client() = delete;
    socket zsock;
    asocket asock;
    bool close;

    static expect<async_client> make(boost::asio::io_context& io, socket zsock);
  };

  class read_msg_op
  {
    async_client* sock_;
    std::string* msg_;

  public:
    read_msg_op(async_client& sock, std::string& msg)
      : sock_(std::addressof(sock)), msg_(std::addressof(msg))
    {}

    template<typename F>
    void operator()(F& self, const boost::system::error_code error = {}, std::size_t = 0)
    {
      if (error)
        return self.complete(error, 0);
      if (!sock_)
        return;
      if (sock_->close)
        return self.complete(boost::asio::error::operation_aborted, 0);

      assert(sock_->zsock && sock_->asock);
      expect<std::string> msg = receive(sock_->zsock.get(), ZMQ_DONTWAIT);
      if (!msg)
      {
        if (msg != make_error_code(EAGAIN))
          return self.complete(make_error_code(msg.error()), 0);

        // try again
        sock_->asock->async_wait(boost::asio::posix::stream_descriptor::wait_read, std::move(self));
        return;
      }

      *msg_ = std::move(*msg);
      self.complete(error, msg_->size());
    }
  };

  class write_msg_op
  {
    async_client* sock_;
    epee::byte_slice msg_;

  public:
    write_msg_op(async_client& sock, epee::byte_slice msg)
      : sock_(std::addressof(sock)), msg_(std::move(msg))
    {}

    template<typename F>
    void operator()(F& self, const boost::system::error_code error = {}, std::size_t = 0)
    {
     if (error)
        return self.complete(error, 0);
      if (!sock_)
        return;
      if (sock_->close)
        return self.complete(boost::asio::error::operation_aborted, 0);

      assert(sock_->zsock && sock_->asock);

      expect<void> status =
        ::net::zmq::send(msg_.clone(), sock_->zsock.get(), ZMQ_DONTWAIT);
      if (!status)
      {
        if (status != ::net::zmq::make_error_code(EAGAIN))
          return self.complete(make_error_code(status.error()), 0);

        // try again
        sock_->asock->async_wait(boost::asio::posix::stream_descriptor::wait_write, std::move(self));
        return;
      }

      self.complete(error, msg_.size());
    }
  };

  //! Cannot have an `async_read` and `async_write` at same time (edge trigger)
  template<typename F>
  auto async_read(async_client& sock, std::string& buffer, F&& f)
  {
    // async_compose is required for correct strand invocation, etc
    return boost::asio::async_compose<F, void(boost::system::error_code, std::size_t)>(
      read_msg_op{sock, buffer}, f, *sock.asock
    );
  }

  //! Cannot have an `async_write` and `async_read` at same time (edge trigger)
  template<typename F>
  auto async_write(async_client& sock, epee::byte_slice msg, F&& f)
  {
    // async_compose is required for correct strand invocation, etc
    return boost::asio::async_compose<F, void(boost::system::error_code, std::size_t)>(
      write_msg_op{sock, std::move(msg)}, f, *sock.asock
    );
  }

}} // net // zmq

