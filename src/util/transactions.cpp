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

#include "carrot_core/account_secrets.h"     // monero/src
#include "carrot_core/address_utils.h"       // monero/src
#include "carrot_core/device.h"              // monero/src
#include "carrot_core/device_ram_borrowed.h" // monero/src
#include "carrot_core/enote_utils.h"         // monero/src
#include "carrot_impl/address_device_ram_borrowed.h" // monero/src
#include "carrot_impl/format_utils.h"     // monero/src
#include "carrot_impl/key_image_device_composed.h"   // monero/src
#include "cryptonote_config.h"            // monero/src
#include "crypto/crypto.h"                // monero/src
#include "crypto/hash.h"                  // monero/src
#include "db/data.h"
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

std::optional<crypto::key_image> lws::get_image(const db::output& source, const carrot::key_image_device& imager, const carrot::view_incoming_key_device& incoming, const crypto::secret_key& balance_key)
{
  try
  {
    if (!source.is_carrot())
      return std::nullopt;

    mx25519_pubkey shared{};
    carrot::view_tag_t tag{};
    const bool is_coinbase = db::unpack(source.extra).first & db::coinbase_output; 
    const mx25519_pubkey de = carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public);
    const carrot::input_context_t context =
      is_coinbase ?
        carrot::make_carrot_input_context_coinbase(std::uint64_t(source.link.height)) :
        carrot::make_carrot_input_context(source.first_image);

    if (source.spend_meta.mixin_count == db::carrot_external)
    {
      if (!incoming.view_key_scalar_mult_x25519(carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public), shared))
        return std::nullopt;
    }
    else
      shared = carrot::raw_byte_convert<mx25519_pubkey>(unwrap(unwrap(balance_key)));

    carrot::make_carrot_view_tag(shared.data, context, source.pub, tag);

    if (is_coinbase)
    {
      return imager.derive_key_image(
        carrot::CarrotCoinbaseOutputOpeningHintV1{
          carrot::CarrotCoinbaseEnoteV1{
            source.pub,
            source.spend_meta.amount,
            source.anchor,
            tag,
            de,
            std::uint64_t(source.link.height)
          },
          carrot::AddressDeriveType::Carrot
        }
      );
    }

    // else !coinbase

    crypto::hash ctx{}; 
    carrot::make_carrot_sender_receiver_secret(shared.data, de, context, ctx);

    return imager.derive_key_image(
      carrot::CarrotOutputOpeningHintV1{
        carrot::CarrotEnoteV1{
          source.pub,
          rct::commit(source.spend_meta.amount, source.ringct_mask),
          carrot::encrypt_carrot_amount(source.spend_meta.amount, ctx, source.pub),
          source.anchor,
          tag,
          de,
          source.first_image
        },
        std::nullopt,
        carrot::subaddress_index_extended{
          {std::uint32_t(source.recipient.maj_i), std::uint32_t(source.recipient.min_i)},
          carrot::AddressDeriveType::Carrot
        }
      }
    );
  }
  catch (const carrot::device_error& e)
  {
    MWARNING("Failed to compute key image: " << e.what());
  }

  return std::nullopt;
}

std::optional<crypto::key_image> lws::get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key, const crypto::secret_key& image_key, const crypto::secret_key& address_key, const crypto::secret_key& incoming_key)
{
  const crypto::public_key account_view =
    rct2pk(rct::scalarmultKey(rct::pk2rct(primary.spend_public), rct::sk2rct(incoming_key)));

  const carrot::view_incoming_key_ram_borrowed_device incoming_device{incoming_key};
  const carrot::generate_image_key_ram_borrowed_device image_device{image_key};
  const carrot::carrot_hierarchy_address_device_ram_borrowed carrot_device{
    primary.spend_public, account_view, primary.view_public, address_key
  };
  const carrot::hybrid_hierarchy_address_device_composed hybrid_device{
    nullptr, std::addressof(carrot_device)
  };
  const carrot::view_balance_secret_ram_borrowed_device balance_device{balance_key};
  const carrot::key_image_device_composed final_device{
    image_device, hybrid_device, std::addressof(balance_device), std::addressof(incoming_device)
  };
  return get_image(source, final_device, incoming_device, balance_key);
}

std::optional<crypto::key_image> lws::get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key)
{
  crypto::secret_key image_key{};
  crypto::secret_key address_key{};
  crypto::secret_key incoming_key{};

  carrot::make_carrot_generateimage_key(balance_key, image_key);
  carrot::make_carrot_generateaddress_secret(balance_key, address_key);
  carrot::make_carrot_viewincoming_key(balance_key, incoming_key);

  return get_image(source, primary, balance_key, image_key, address_key, incoming_key);
}
