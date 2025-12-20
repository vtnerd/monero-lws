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

#include "carrot.h"

#include "carrot_core/account_secrets.h"     // monero/src
#include "carrot_core/address_utils.h"       // monero/src
#include "carrot_core/device.h"              // monero/src
#include "carrot_core/device_ram_borrowed.h" // monero/src
#include "carrot_core/enote_utils.h"         // monero/src
#include "carrot_impl/address_device_ram_borrowed.h" // monero/src
#include "carrot_impl/format_utils.h"     // monero/src
#include "carrot_impl/key_image_device_composed.h"   // monero/src:w
#include "db/data.h"
#include "ringct/rctOps.h"               // monero/src

namespace lws { namespace carrot
{
  //! "Wallet" pubkeys as specified by carrot documentation
  account::account(const db::account& source)
    : spend(source.address.spend_public)
  {
    unwrap(unwrap(incoming)) = source.key;
    if (source.flags & db::view_balance_key)
    {
      const crypto::secret_key balance = incoming;
      ::carrot::make_carrot_viewincoming_key(balance, incoming);
    }

    view = rct2pk(rct::scalarmultKey(rct::pk2rct(spend), rct::sk2rct(incoming)));
  }

  std::optional<crypto::key_image> get_image(const db::output& source, const ::carrot::key_image_device& imager, const ::carrot::view_incoming_key_device& incoming, const crypto::secret_key& balance_key)
  {
    try
    {
      if (!source.is_carrot())
        return std::nullopt;

      mx25519_pubkey shared{};
      ::carrot::view_tag_t tag{};
      const bool is_coinbase = db::unpack(source.extra).first & db::coinbase_output; 
      const mx25519_pubkey de = ::carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public);
      const ::carrot::input_context_t context =
        is_coinbase ?
          ::carrot::make_carrot_input_context_coinbase(std::uint64_t(source.link.height)) :
          ::carrot::make_carrot_input_context(source.first_image);

      if (source.spend_meta.mixin_count == db::carrot_external)
      {
        if (!incoming.view_key_scalar_mult_x25519(::carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public), shared))
          return std::nullopt;
      }
      else
        shared = ::carrot::raw_byte_convert<mx25519_pubkey>(unwrap(unwrap(balance_key)));

      ::carrot::make_carrot_view_tag(shared.data, context, source.pub, tag);

      if (is_coinbase)
      {
        return imager.derive_key_image(
          ::carrot::CarrotCoinbaseOutputOpeningHintV1{
            ::carrot::CarrotCoinbaseEnoteV1{
              source.pub,
              source.spend_meta.amount,
              source.anchor,
              tag,
              de,
              std::uint64_t(source.link.height)
            },
            ::carrot::AddressDeriveType::Carrot
          }
        );
      }

      // else !coinbase

      crypto::hash ctx{}; 
      ::carrot::make_carrot_sender_receiver_secret(shared.data, de, context, ctx);

      return imager.derive_key_image(
        ::carrot::CarrotOutputOpeningHintV1{
          ::carrot::CarrotEnoteV1{
            source.pub,
            rct::commit(source.spend_meta.amount, source.ringct_mask),
            ::carrot::encrypt_carrot_amount(source.spend_meta.amount, ctx, source.pub),
            source.anchor,
            tag,
            de,
            source.first_image
          },
          std::nullopt,
          ::carrot::subaddress_index_extended{
            {std::uint32_t(source.recipient.maj_i), std::uint32_t(source.recipient.min_i)},
            ::carrot::AddressDeriveType::Carrot
          }
        }
      );
    }
    catch (const ::carrot::device_error& e)
    {
      MWARNING("Failed to compute key image: " << e.what());
    }

    return std::nullopt;
  }

  std::optional<crypto::key_image> get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key, const crypto::secret_key& image_key, const crypto::secret_key& address_key, const crypto::secret_key& incoming_key)
  {
    const crypto::public_key account_view =
      rct2pk(rct::scalarmultKey(rct::pk2rct(primary.spend_public), rct::sk2rct(incoming_key)));

    const auto incoming_device = std::make_shared<::carrot::view_incoming_key_ram_borrowed_device>(incoming_key);
    const auto image_device = std::make_shared<::carrot::generate_image_key_ram_borrowed_device>(image_key);
    const auto generate_device = std::make_shared<::carrot::generate_address_secret_ram_borrowed_device>(address_key);
    const auto carrot_device = std::make_shared<::carrot::carrot_hierarchy_address_device>(
      generate_device, primary.spend_public, account_view
    );
    const auto hybrid_device = std::make_shared<::carrot::hybrid_hierarchy_address_device>(
      carrot_device, nullptr
    );
    const auto balance_device = std::make_shared<::carrot::view_balance_secret_ram_borrowed_device>(balance_key);
    const ::carrot::key_image_device_composed final_device{
      image_device, hybrid_device, balance_device, incoming_device
    };
    return get_image(source, final_device, *incoming_device, balance_key);
  }

  std::optional<crypto::key_image> get_image(const db::output& source, const db::account_address& primary, const crypto::secret_key& balance_key)
  {
    crypto::secret_key image_key{};
    crypto::secret_key address_key{};
    crypto::secret_key incoming_key{};

    ::carrot::make_carrot_generateimage_key(balance_key, image_key);
    ::carrot::make_carrot_generateaddress_secret(balance_key, address_key);
    ::carrot::make_carrot_viewincoming_key(balance_key, incoming_key);

    return get_image(source, primary, balance_key, image_key, address_key, incoming_key);
  }
}} // lws // carrot

