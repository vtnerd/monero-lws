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

#include "zmq_async.h"

#include <stdexcept>

namespace net { namespace zmq
{
  const boost::system::error_category& boost_error_category() noexcept
  {
    struct category final : boost::system::error_category
    {
      virtual const char* name() const noexcept override final
      {
        return "error::error_category()";
      }

      virtual std::string message(int value) const override final
      {
        char const* const msg = zmq_strerror(value);
        if (msg)
          return msg;
        return "zmq_strerror failure";
      }

      virtual boost::system::error_condition default_error_condition(int value) const noexcept override final
      {
        // maps specific errors to generic `std::errc` cases.
        switch (value)
        {
        case EFSM:
        case ETERM:
          break;
        default:
          /* zmq is using cerrno errors. C++ spec indicates that `std::errc`
            values must be identical to the cerrno value. So just map every zmq
            specific error to the generic errc equivalent. zmq extensions must
            be in the switch or they map to a non-existent errc enum value. */
          return boost::system::errc::errc_t(value);
        }
        return boost::system::error_condition{value, *this};
      }
    };
    static const category instance{};
    return instance;
  }

  boost::system::error_code make_error_code(std::error_code code)
  {
    if (std::addressof(code.category()) != std::addressof(error_category()))
      throw std::logic_error{"Expected only ZMQ errors"};
    return boost::system::error_code{code.value(), boost_error_category()};
  }

  void free_descriptor::operator()(adescriptor* ptr) noexcept
  {
    if (ptr)
    {
      ptr->release(); // release ASIO ownership, destroys all queued handlers
      delete ptr;
    }
  }

  expect<async_client> async_client::make(boost::asio::io_context& io, socket zsock)
  {
    MONERO_PRECOND(zsock != nullptr);

    int fd = 0;
    std::size_t length = sizeof(fd);
    if (zmq_getsockopt(zsock.get(), ZMQ_FD, &fd, &length) != 0)
      return net::zmq::get_error_code();

    async_client out{std::move(zsock), nullptr, false};
    out.asock.reset(new adescriptor{io, fd});
    return out;
  }
}} // net // zmq

