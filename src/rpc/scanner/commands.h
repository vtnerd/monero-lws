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

#include <boost/endian/buffers.hpp>
#include <cstdint>
#include <vector>

#include "crypto/hash.h" // monero/src
#include "db/fwd.h"
#include "wire/msgpack/fwd.h"

namespace lws { namespace rpc { namespace scanner
{
  /* 
  `monero-lws-daemon` is "server" and `monero-lws-scanner` is "client".
  */

  //! \brief Data sent before msgpack payload
  struct header
  {
    using length_type = boost::endian::little_uint32_buf_t;
    header() = delete;
    std::uint8_t version;
    std::uint8_t id;
    length_type length;
  };
  static_assert(sizeof(header) == 6);


  /*
   Client to server messages.
  */

  //! \brief Inform server of info needed to spawn work to client.
  struct initialize
  {
    initialize() = delete;
    static constexpr std::uint8_t id() noexcept { return 0; }
    std::string pass;
    std::uint32_t threads;
  };
  WIRE_MSGPACK_DECLARE_OBJECT(initialize);

  //! Command that updates database account records
  struct update_accounts
  {
    update_accounts() = delete;
    static constexpr std::uint8_t id() noexcept { return 1; }
    std::vector<lws::account> users;
    std::vector<crypto::hash> blocks;
  };
  WIRE_MSGPACK_DECLARE_OBJECT(update_accounts); 


  /*
   Server to client messages.
  */
 
  //! \brief New accounts to add/push to scanning list
  struct push_accounts
  {
    push_accounts() = delete;
    static constexpr std::uint8_t id() noexcept { return 0; }
    std::vector<lws::account> users;
  };
  WIRE_MSGPACK_DECLARE_OBJECT(push_accounts);

  //! \brief Replace account scanning list with this new one
  struct replace_accounts
  {
    replace_accounts() = delete;
    static constexpr const std::uint8_t id() noexcept { return 1; }
    std::vector<lws::account> users;
  };
  WIRE_MSGPACK_DECLARE_OBJECT(replace_accounts);
}}} // lws // rpc // scanner
