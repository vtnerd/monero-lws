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

#include "write_commands.h"

#include <cstring>
#include <limits>

#include "byte_slice.h" // monero/contrib/epee/include

namespace
{
  constexpr const std::size_t max_write_commands = 1000;
}

namespace lws { namespace rpc { namespace scanner
{ 
  write_status write_command(const std::shared_ptr<connection>& self, const std::uint8_t id, epee::byte_stream sink)
  {
    if (!self)
      return write_status::failed;

    if (max_write_commands <= self->write_bufs_.size())
    {
      MERROR("Exceeded max write queue size for " << self->remote_address());
      return write_status::failed;
    }

    if (sink.size() < sizeof(header))
    {
      MERROR("Message sink was unexpectedly shrunk on message to " << self->remote_address());
      return write_status::failed;
    }

    using value_type = header::length_type::value_type;
    if (std::numeric_limits<value_type>::max() < sink.size() - sizeof(header))
    {
      MERROR("Message to " << self->remote_address() << " exceeds max size");
      return write_status::failed;
    }

    const header head{id, header::length_type{value_type(sink.size() - sizeof(header))}};
    std::memcpy(sink.data(), std::addressof(head), sizeof(head));

    const bool rc = self->write_bufs_.empty();
    self->write_bufs_.push_back(epee::byte_slice{std::move(sink)});
    return rc ? write_status::needs_queueing : write_status::already_queued;
  }
}}} // lws // rpc // scanner
