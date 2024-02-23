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

#include "lws_pub.h"

#include <boost/range/adaptor/transformed.hpp>
#include "db/account.h"
#include "rpc/client.h"
#include "rpc/webhook.h"
#include "wire/crypto.h"
#include "wire/wrapper/array.h"
#include "wire/wrappers_impl.h"
#include "wire/write.h"

namespace
{
  constexpr const char json_topic[] = "json-minimal-scanned:";
  constexpr const char msgpack_topic[] = "msgpack-minimal-scanned:";

  struct get_address
  {
    std::reference_wrapper<const lws::account> user;
  };

  void write_bytes(wire::writer& dest, const get_address& self)
  {
    wire_write::bytes(dest, self.user.get().address());
  }

  struct minimal_scanned
  {
    std::reference_wrapper<const crypto::hash> id;
    const lws::db::block_id height;
    epee::span<const lws::account> users;
  };

  void write_bytes(wire::writer& dest, const minimal_scanned& self)
  {
    const auto just_address = [] (const lws::account& user)
    {
      return get_address{std::cref(user)};
    };

    wire::object(dest,
      WIRE_FIELD(height),
      WIRE_FIELD(id),
      wire::field("addresses", wire::array(self.users | boost::adaptors::transformed(just_address)))
    );
  }
} // anonymous

namespace lws { namespace rpc
{
  void publish_scanned(rpc::client& client, const crypto::hash& id, epee::span<const account> users)
  {
    if (users.empty())
      return;

    const minimal_scanned output{
      std::cref(id), users[0].scan_height(), users
    };
    const epee::span<const minimal_scanned> event{std::addressof(output), 1};
    zmq_send(client, event, json_topic, msgpack_topic);
  }
}} // lws // rpc
