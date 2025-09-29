// Copyright (c) 2025, The Monero Project
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

#include <memory>
#include <optional>
#include "db/fwd.h"
#include "crypto/crypto.h" // monero/src
#include "wire/fwd.h"

namespace carrot
{
  struct janus_anchor_t;
  using encrypted_janus_anchor_t = janus_anchor_t;
  class key_image_device;
  class view_incoming_key_device;
}
namespace lws { namespace carrot
{
  //! From the carrot hierarchy
  struct balance_secrets
  {
    crypto::secret_key kgi; //!< generate-image key
    crypto::secret_key kv;  //!< incoming view key
    crypto::secret_key sga; //!< generate-address secret

    balance_secrets(const db::account_pubs& pubs, const crypto::secret_key& svb);
    balance_secrets()
      : kgi{}, kv{}, sga{}
    {}
  };
  WIRE_DECLARE_OBJECT(balance_secrets);

  //! View public differs from "address" account - see carrot docs
  struct account 
  {
    crypto::public_key view_public;
    crypto::public_key spend_public;

    explicit account(const db::account& source);
    explicit account(const db::account& source, const balance_secrets* secrets);

    explicit account(const db::account& source, std::unique_ptr<const balance_secrets> secrets)
      : account(source, secrets.get())
    {}

    account() noexcept
      : view_public{}, spend_public{}
    {}
  };

  std::optional<crypto::key_image> get_image(const db::output& source, const ::carrot::key_image_device& imager, const ::carrot::view_incoming_key_device& incoming, const crypto::secret_key& balance_key);
  std::optional<crypto::key_image> get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key, const carrot::balance_secrets& secrets);
}} // lws // carrot
