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

#include <cstdint>
#include <optional>
#include <utility>

#include "common/pod-class.h" // monero/src
#include "crypto/crypto.h"    // monero/src
#include "db/fwd.h"
#include "ringct/rctTypes.h"  // moneor/src 

namespace carrot
{
  struct janus_anchor_t;
  using encrypted_janus_anchor_t = janus_anchor_t;
  class key_image_device;
  class view_incoming_key_device;
}
namespace lws
{
  void decrypt_payment_id(crypto::hash8& out, const crypto::key_derivation& key);
  std::optional<std::pair<std::uint64_t, rct::key>> decode_amount(const rct::key& commitment, const rct::ecdhTuple& info, const crypto::key_derivation& sk, std::size_t index, const bool bulletproof2);

  std::optional<crypto::key_image> get_image(const db::output& source, const carrot::key_image_device& imager, const carrot::view_incoming_key_device& incoming, const crypto::secret_key& balance_key);
  std::optional<crypto::key_image> get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key, const crypto::secret_key& image_key, const crypto::secret_key& address_key, const crypto::secret_key& incoming_key);
  std::optional<crypto::key_image> get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key);
}
