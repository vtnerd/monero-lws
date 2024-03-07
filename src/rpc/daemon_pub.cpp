// Copyright (c) 2020, The Monero Project
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

#include "daemon_pub.h"

#include "cryptonote_basic/cryptonote_basic.h" // monero/src
#include "rpc/daemon_zmq.h"
#include "wire/crypto.h"
#include "wire/error.h"
#include "wire/field.h"
#include "wire/traits.h"
#include "wire/json/read.h"
#include "wire/wrapper/array.h"
#include "wire/wrappers_impl.h"

namespace
{
  using max_txes_pub = wire::max_element_count<775>;

  struct dummy_chain_array
  {
    using value_type = crypto::hash;

    std::size_t count = 0;
    std::reference_wrapper<crypto::hash> id;

    void clear() noexcept {}
    void reserve(std::size_t) noexcept {}

    std::size_t size() const noexcept { return count; }
    crypto::hash& back() noexcept { return id; }
    void emplace_back() { ++count; }
  };
}

namespace wire
{
  template<>
  struct is_array<dummy_chain_array>
    : std::true_type
  {};
}

namespace lws
{
namespace rpc
{
  static void read_bytes(wire::json_reader& src, minimal_chain_pub& self)
  {
    dummy_chain_array chain{0, std::ref(self.top_block_id)};
    wire::object(src,
      wire::field("first_height", std::ref(self.top_block_height)),
      wire::field("ids", std::ref(chain))
    );

    self.top_block_height += chain.count - 1;
    if (chain.count == 0)
      WIRE_DLOG_THROW(wire::error::schema::binary, "expected at least one block hash");
  }

  expect<minimal_chain_pub> minimal_chain_pub::from_json(std::string&& source)
  {
    minimal_chain_pub out{};
    std::error_code err = wire::json::from_bytes(std::move(source), out);
    if (err)
      return err;
    return {std::move(out)};
  }

  static void read_bytes(wire::json_reader& source, full_txpool_pub& self)
  {
    wire_read::bytes(source, wire::array<max_txes_pub>(std::ref(self.txes)));
  }

  expect<full_txpool_pub> full_txpool_pub::from_json(std::string&& source)
  {
    full_txpool_pub out{};
    std::error_code err = wire::json::from_bytes(std::move(source), out);
    if (err)
      return err;
    return {std::move(out)};
  }
}
}
