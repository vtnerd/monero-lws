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

#include "transactions.h"

#include "carrot_core/device.h"           // monero/src
#include "carrot_impl/key_image_device.h" // monero/src
#include "cryptonote_config.h"            // monero/src
#include "crypto/crypto.h"                // monero/src
#include "crypto/hash.h"                  // monero/src
#include "misc_log_ex.h"                  // monero/contrib/include
#include "ringct/rctOps.h"                // monero/src

void lws::decrypt_payment_id(crypto::hash8& out, const crypto::key_derivation& key)
{
  crypto::hash hash;
  char data[33]; /* A hash, and an extra byte */

  memcpy(data, &key, 32);
  data[32] = config::HASH_KEY_ENCRYPTED_PAYMENT_ID;
  cn_fast_hash(data, 33, hash);

  for (size_t b = 0; b < 8; ++b)
    out.data[b] ^= hash.data[b];
}

std::optional<std::pair<std::uint64_t, rct::key>> lws::decode_amount(const rct::key& commitment, const rct::ecdhTuple& info, const crypto::key_derivation& sk, std::size_t index, const bool bulletproof2)
{
  crypto::secret_key scalar{};
  crypto::derivation_to_scalar(sk, index, scalar);

  rct::ecdhTuple copy{info};
  rct::ecdhDecode(copy, rct::sk2rct(scalar), bulletproof2);

  rct::key Ctmp;
  rct::addKeys2(Ctmp, copy.mask, copy.amount, rct::H);
  if (rct::equalKeys(commitment, Ctmp))
    return {{rct::h2d(copy.amount), copy.mask}};
  return std::nullopt;
}

std::optional<crypto::key_image> lws::get_image(const db::output& source, const carrot::key_image_device& imager)
{
  try
  {
/*
struct CarrotOutputOpeningHintV2
{
    /// K_o
    crypto::public_key onetime_address;
    /// C_a
    rct::key amount_commitment;
    /// anchor_enc
    encrypted_janus_anchor_t anchor_enc;
    /// view_tag
    view_tag_t view_tag;
    /// D_e
    mx25519_pubkey enote_ephemeral_pubkey;
    /// L_0
    crypto::key_image tx_first_key_image;

    /// a
    rct::xmr_amount amount;

    // pid_enc
    std::optional<encrypted_payment_id_t> encrypted_payment_id;

    // j, derive type
    subaddress_index_extended subaddr_index;
};

    return imager.derive_key_image(
      carrot::CarrotOutputOpeningHintV2{
       source.pub,
      }
    ); */
  }
  catch (const carrot::device_error& e)
  {
    MWARNING("Failed to compute key image: " << e.what());
  }
  return std::nullopt;
}
