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

#include <boost/asio/coroutine.hpp>
#include <boost/asio/read.hpp>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "byte_slice.h"  // monero/contrib/epee/include
#include "db/account.h"
#include "misc_log_ex.h"
#include "rpc/scanner/connection.h"
#include "wire/msgpack/base.h"
#include "wire/msgpack/read.h"

namespace lws { namespace rpc { namespace scanner
{
  /*! Function for binding to command callables.

    \tparam F concept requirements:
      * Must have inner `typedef` named `input` which specifies a type
        that can read from msgpack bytes.
      * Must have static function `handle` with interface
        `bool(std::shared_ptr<T>, F::input)`.
    \tparam T concept requirements:
      * Must be derived from `lws::rpc::scanner::connection`. */
  template<typename F, typename T>
  bool call(const std::shared_ptr<T>& self)
  {
    static_assert(std::is_base_of<connection, T>{});
    if (!self)
      return false;

    typename F::input data{};
    const std::error_code error = 
      wire::msgpack::from_bytes(epee::byte_slice{std::move(self->read_buf_)}, data);
    self->read_buf_.clear();
    if (error)
    {
      MERROR("Failed to unpack message (from " << self->remote_address() << "): " << error.message());
      return false;
    }

    return F::handle(self, std::move(data));
  }
 
  /*! \brief ASIO coroutine for reading remote client OR server commands.

    \tparam T concept requirements:
      * Must be derived from `lws::rpc::scanner::connection`.
      * Must have `cleanup()` function that invokes `base_cleanup()`, and
        does any other necessary work given that the socket connection is being
        terminated.
      * Must have a static `commands()` function, which returns a `std::array`
        of `bool(std::shared_ptr<T>)` callables. The position in the array
        determines the command number. */
  template<typename T>
  class read_commands : public boost::asio::coroutine
  {
    static_assert(std::is_base_of<connection, T>{});
    std::shared_ptr<T> self_;
  public:
    explicit read_commands(std::shared_ptr<T> self)
      : boost::asio::coroutine(), self_(std::move(self))
    {}

    //! Invoke with no arguments to start read commands loop
    void operator()(const boost::system::error_code& error = {}, const std::size_t transferred = 0)
    {
      if (error || !self_)
      {
        if (error != boost::asio::error::operation_aborted)
        {
          const std::string remote = self_ ?
            self_->remote_address() : std::string{};
          MERROR("Read error on socket (" << remote << "): " << error.message());
          if (self_)
            self_->cleanup();
        }
        return;
      }
      if (self_->cleanup_)
        return; // callback queued before cancellation

      BOOST_ASIO_CORO_REENTER(*this)
      {
        for (;;) // multiple commands
        {
          // indefinite read timeout (waiting for next command)
          BOOST_ASIO_CORO_YIELD boost::asio::async_read(self_->sock_, self_->read_buffer(sizeof(self_->next_)), *this);

          std::memcpy(std::addressof(self_->next_), self_->read_buf_.data(), sizeof(self_->next_));
          static_assert(std::numeric_limits<header::length_type::value_type>::max() <= std::numeric_limits<std::size_t>::max());
          BOOST_ASIO_CORO_YIELD boost::asio::async_read(self_->sock_, self_->read_buffer(self_->next_.length.value()), *this);

          const auto& commands = T::commands();
          if (commands.size() <= self_->next_.id || !commands[self_->next_.id](self_))
          {
            self_->cleanup();
            return; // stop reading commands
          }
        }
      }
    }
  };
}}} // lws // rpc // scanner
