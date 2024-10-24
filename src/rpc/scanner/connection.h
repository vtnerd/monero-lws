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

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <deque>
#include <string>

#include "byte_slice.h"  // monero/contrib/epee/include
#include "byte_stream.h" // monero/contrib/epee/include
#include "rpc/scanner/commands.h"

namespace lws { namespace rpc { namespace scanner
{
  //! \brief Base class for `client_connection` and `server_connection`. Always use `strand_`.
  struct connection
  {
    // Leave public for coroutines `read_commands` and `write_commands`
    epee::byte_stream read_buf_;
    std::deque<epee::byte_slice> write_bufs_;
    boost::asio::ip::tcp::socket sock_;
    boost::asio::steady_timer write_timeout_;
    boost::asio::io_context::strand strand_;
    header next_;
    bool cleanup_;

    explicit connection(boost::asio::io_context& io);
    ~connection();

    boost::asio::io_context& context() const { return strand_.context(); }

    boost::asio::ip::tcp::endpoint remote_endpoint();

    //! \return ASIO compatible read buffer of `size`.
    boost::asio::mutable_buffer read_buffer(const std::size_t size);

    //! \return ASIO compatible write buffer
    boost::asio::const_buffer write_buffer() const;

    //! Cancels operations on socket and timer. Also updates `cleanup_ = true`.
    void base_cleanup();
  };
}}} // lws // rpc // scanner
