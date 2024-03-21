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
#include <boost/asio/write.hpp>
#include <chrono>
#include <memory>
#include <system_error>
#include <type_traits>
#include <vector>

#include "byte_stream.h"  // monero/contrib/epee/include
#include "commands.h"
#include "crypto/hash.h"  // monero/src
#include "db/account.h"
#include "misc_log_ex.h"
#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "wire/msgpack/write.h"

namespace lws { namespace rpc { namespace scanner
{
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
        MERROR("Write timeout on socket (" << self_->remote_address() << ")");
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
  class write_commands : public boost::asio::coroutine
  {
    static_assert(std::is_base_of<connection, T>{});
    std::shared_ptr<T> self_;
  public:
    explicit write_commands(std::shared_ptr<T> self)
      : boost::asio::coroutine(), self_(std::move(self))
    {}

    void operator()(const boost::system::error_code& error = {}, const std::size_t transferred = 0)
    {
      if (error || !self_)
      {
        if (error != boost::asio::error::operation_aborted)
        {
          const std::string remote = self_ ?
            self_->remote_address() : std::string{};
          MERROR("Write error on socket (" << remote << "): " << error.message());
          if (self_)
            self_->cleanup();
        }
        return;
      }
      if (self_->cleanup_)
        return; // callback queued before cancellation

      BOOST_ASIO_CORO_REENTER(*this)
      {
        while (!self_->write_bufs_.empty())
        {
          self_->write_timeout_.expires_from_now(std::chrono::seconds{10});
          self_->write_timeout_.async_wait(timeout<T>{self_});
          BOOST_ASIO_CORO_YIELD boost::asio::async_write(self_->sock_, self_->write_buffer(), *this);
          self_->write_timeout_.cancel();
          self_->write_bufs_.pop_front(); 
        }
      }
    }
  };

  enum class write_status
  {
    already_queued, failed, needs_queueing
  };

  //! Completes writing command to `sink`, and queues for writing on `self`
  write_status write_command(const std::shared_ptr<connection>& self, std::uint8_t id, epee::byte_stream sink);

  /*! Writes "raw" `header` then `data` as msgpack, and queues for writing to
    `self`. Also starts ASIO async writing (via `write_commands`) if the queue
    was empty before queueing `data`.

    \tparam T must meet concept requirements for `T` outlined in
      `write_commands`.
    \tparam U concept requirements:
      * must be serializable to msgpack using `wire` engine.
      * must have static function `id` which returns an `std::uint8_t` to
        identify the command on the remote side. */
  template<typename T, typename U>
  bool write_command(std::shared_ptr<T> self, const U& data)
  {
    static_assert(std::is_base_of<connection, T>{});
    if (!self)
      return false;

    epee::byte_stream sink{};
    sink.put_n(0, sizeof(header));

    {
      // use integer keys for speed (default to_bytes uses strings)
      wire::msgpack_slice_writer dest{std::move(sink), true};
      try
      {
        wire_write::bytes(dest, data);
      }
      catch (const wire::exception& e)
      {
        MERROR("Failed to serialize msgpack for remote (" << self->remote_address() << ") command: " << e.what());
        return false;
      }
      sink = dest.take_sink();
    }

    switch (write_command(self, U::id(), std::move(sink)))
    {
    case write_status::already_queued:
      break;
    case write_status::failed:
      return false;
    case write_status::needs_queueing:
      write_commands<T>{std::move(self)}();
      break;
    }
    return true;
  }
}}} // lws // rpc // scanner
