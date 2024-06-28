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

#include "connection.h"

#include "misc_log_ex.h" // monero/contrib/epee/include

namespace lws { namespace rpc { namespace scanner
{
  connection::connection(boost::asio::io_service& io)
    : read_buf_(),
      write_bufs_(),
      sock_(io),
      write_timeout_(io),
      strand_(io),
      next_{},
      cleanup_(false)
  {}

  connection::~connection()
  {}

  boost::asio::ip::tcp::endpoint connection::remote_endpoint()
  {
    boost::system::error_code error{};
    return sock_.remote_endpoint(error);
  }

  boost::asio::mutable_buffer connection::read_buffer(const std::size_t size)
  {
    assert(strand_.running_in_this_thread());
    read_buf_.clear();
    read_buf_.put_n(0, size);
    return boost::asio::mutable_buffer(read_buf_.data(), size);
  }

  boost::asio::const_buffer connection::write_buffer() const
  {
    assert(strand_.running_in_this_thread());
    if (write_bufs_.empty())
      return boost::asio::const_buffer(nullptr, 0);
    return boost::asio::const_buffer(write_bufs_.front().data(), write_bufs_.front().size());
  }

  void connection::base_cleanup()
  {
    assert(strand_.running_in_this_thread());
    if (!cleanup_)
      MINFO("Disconnected from " << remote_endpoint() << " / " << this);
    cleanup_ = true;
    boost::system::error_code error{};
    sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    error = boost::system::error_code{};
    sock_.close(error);
    if (error)
      MERROR("Error when closing socket: " << error.message());
    write_timeout_.cancel();
  }
}}} // lws // rpc // scanner
