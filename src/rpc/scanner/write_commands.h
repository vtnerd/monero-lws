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
#include <boost/asio/dispatch.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <memory>
#include <system_error>
#include <type_traits>
#include <vector>

#include "byte_slice.h"   // monero/contrib/epee/include
#include "byte_stream.h"  // monero/contrib/epee/include
#include "commands.h"
#include "common/expect.h"// monero/src
#include "crypto/hash.h"  // monero/src
#include "db/account.h"
#include "misc_log_ex.h"
#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "wire/msgpack/write.h"

namespace lws { namespace rpc { namespace scanner
{
  constexpr const std::size_t max_write_buffers = 100;

  /* \brief ASIO handler for write timeouts

    \tparam T concept requirements:
      * Must be derived from `lws::rpc::scanner::connection`.
      * Must have `cleanup()` function that invokes `base_cleanup()`, and
        does any other necessary work given that the socket connection is being
        terminated. */
  template<typename T>
  struct timeout
  {
    static_assert(std::is_base_of<connection, T>{});
    std::shared_ptr<T> self_;

    void operator()(const boost::system::error_code& error) const
    {
      if (self_ && error != boost::asio::error::operation_aborted)
      {
        assert(self_->strand_.running_in_this_thread());
        MERROR("Write timeout on socket (" << self_->remote_endpoint() << ")");
        self_->cleanup();
      }
    }
  };

  /*! \brief ASIO coroutine for write client OR server commands.

    \tparam T concept requirements:
      * Must be derived from `lws::rpc::scanner::connection`.
      * Must have `cleanup()` function that invokes `base_cleanup()`, and
        does any other necessary work given that the socket connection is being
        terminated. */
  template<typename T>
  class write_buffers : public boost::asio::coroutine
  {
    static_assert(std::is_base_of<connection, T>{});
    std::shared_ptr<T> self_;
  public:
    explicit write_buffers(std::shared_ptr<T> self)
      : boost::asio::coroutine(), self_(std::move(self))
    {}

    write_buffers(write_buffers&&) = default;
    write_buffers(const write_buffers&) = default;

    void operator()(const boost::system::error_code& error = {}, std::size_t = 0)
    {
      if (!self_)
        return;

      assert(self_->strand_.running_in_this_thread());
      if (error)
      {
        if (error != boost::asio::error::operation_aborted)
        {
          MERROR("Write error on socket (" << self_->remote_endpoint() << "): " << error.message());
          self_->cleanup();
        }
        self_->write_timeout_.cancel();
        return;
      }
      if (self_->cleanup_)
        return; // callback queued before cancellation

      BOOST_ASIO_CORO_REENTER(*this)
      {
        while (!self_->write_bufs_.empty())
        {
          self_->write_timeout_.expires_from_now(std::chrono::seconds{10});
          self_->write_timeout_.async_wait(self_->strand_.wrap(timeout<T>{self_}));
          BOOST_ASIO_CORO_YIELD boost::asio::async_write(self_->sock_, self_->write_buffer(), self_->strand_.wrap(*this));
          self_->write_timeout_.cancel();
          self_->write_bufs_.pop_front(); 
        }
      }
    }
  };

  //! \return Completed message using `sink` as source.
  epee::byte_slice complete_command(std::uint8_t id, epee::byte_stream sink);


  /*! Writes "raw" `header` then `data` as msgpack, and queues for writing to
    `self`. Also starts ASIO async writing (via `write_buffers`) if the queue
    was empty before queueing `data`.

    \tparam T must meet concept requirements for `T` outlined in
      `write_commands`.
    \tparam U concept requirements:
      * must be serializable to msgpack using `wire` engine.
      * must have static function `id` which returns an `std::uint8_t` to
        identify the command on the remote side. */
  template<typename T, typename U>
  void write_command(const std::shared_ptr<T>& self, const U& data)
  {
    static_assert(std::is_base_of<connection, T>{});
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");

    epee::byte_slice msg = nullptr;
    try
    {
      epee::byte_stream sink{};
      sink.put_n(0, sizeof(header));

      // use integer keys for speed (default to_bytes uses strings)
      wire::msgpack_slice_writer dest{std::move(sink), true};
      wire_write::bytes(dest, data);

      msg = complete_command(U::id(), dest.take_sink()); 
    }
    catch (const wire::exception& e)
    {
      MERROR("Failed to serialize msgpack for remote (" << self.get() << ") command: " << e.what());
      throw; // this should rarely happen, so just shutdown
    }

    if (msg.empty())
    {
      boost::asio::dispatch(self->strand_, [self] () { self->cleanup(); });
      return;
    }

    class queue_slice
    {
      std::shared_ptr<T> self_;
      epee::byte_slice msg_;

    public:
      explicit queue_slice(std::shared_ptr<T> self, epee::byte_slice msg)
        : self_(std::move(self)), msg_(std::move(msg))
      {}

      queue_slice(queue_slice&&) = default;
      queue_slice(const queue_slice& rhs)
        : self_(rhs.self_), msg_(rhs.msg_.clone())
      {}

      void operator()()
      {
        if (!self_)
          return;

        const bool queue = self_->write_bufs_.empty();
        self_->write_bufs_.push_back(std::move(msg_));

        if (queue)
          write_buffers{self_}();
        else if (max_write_buffers <= self_->write_bufs_.size())
        {
          MERROR("Exceeded max buffer size for connection: " << self_->remote_endpoint());
          self_->cleanup();
        }
      }
    };

    self->strand_.dispatch(queue_slice{self, std::move(msg)});
  }
}}} // lws // rpc // scanner
