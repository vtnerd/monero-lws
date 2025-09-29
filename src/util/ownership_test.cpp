// Copyright (c) 2018-2025, The Monero Project
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

#include "ownership_test.h"

#include <boost/optional/optional.hpp>
#include <boost/range/combine.hpp>

#include "carrot_core/device_ram_borrowed.h"          // monero/src
#include "carrot_core/enote_utils.h"                  // monero/src
#include "carrot_core/scan.h"                         // monero/src
#include "carrot_impl/format_utils.h"                 // monero/src 
#include "common/error.h"
#include "crypto/wallet/crypto.h"                     // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_basic/tx_extra.h"
#include "db/account.h"
#include "db/data.h"
#include "db/storage.h"
#include "error.h"
#include "misc_log_ex.h"
#include "rpc/daemon_messages.h"
#include "util/transactions.h"

namespace lws
{
  namespace
  {
    std::optional<db::address_index> is_match(const crypto::public_key& derived_pub, const account& user, boost::optional<subaddress_reader>& subaddress, const db::block_id height)
    {
      if (user.spend_public() == derived_pub)
        return db::address_index::primary(); // no need to update lookahead

      if (!subaddress)
        return std::nullopt;

      const expect<db::address_index> match =
        subaddress->find_subaddress(user, derived_pub);
      if (!match)
      {
        if (match != lmdb::error(MDB_NOTFOUND))
          MERROR("Failure when doing subaddress search: " << match.error().message());
        return std::nullopt;
      }

      subaddress->update_lookahead(user, *match, height);
      return *match;
    }
  }

  ownership_test::ownership_test(spend_action on_spend, output_action on_output)
    : on_spend(std::move(on_spend))
    , on_output(std::move(on_output))
  {}

  void ownership_test::enable_subaddresses(const db::storage& disk, const std::uint32_t max_subaddresses)
  {
    subaddress.emplace(disk, max_subaddresses);
  }

  void ownership_test::disable_subaddresses()
  {
    subaddress.reset();
  }

  void ownership_test::operator()(
    epee::span<account> users,
    db::block_id height,
    std::uint64_t timestamp,
    crypto::hash const* tx_hash,
    const cryptonote::transaction& tx,
    const std::vector<std::uint64_t>& out_ids,
    const bool is_unified)
  {
    if (2 < tx.version)
      throw std::runtime_error{"Unsupported tx version"};

    struct carrot_secrets
    {
      crypto::secret_key gout;
      crypto::secret_key tout;
      crypto::secret_key blinding;
    };
    std::optional<carrot_secrets> is_carrot;
    if (::carrot::is_carrot_transaction_v1(tx))
      is_carrot.emplace();

    crypto::hash tx_hash_lazy;
    cryptonote::tx_extra_pub_key key;
    boost::optional<crypto::hash> prefix_hash;
    boost::optional<cryptonote::tx_extra_nonce> extra_nonce;
    std::pair<std::uint8_t, db::output::payment_id_> payment_id;
    cryptonote::tx_extra_additional_pub_keys additional_tx_pub_keys;
    std::vector<mx25519_pubkey> additional_derivations;

    const auto get_tx_hash = [&] () -> const crypto::hash&
    {
      if (!tx_hash)
      {
        tx_hash_lazy = get_transaction_hash(tx);
        tx_hash = std::addressof(tx_hash_lazy);
      } 
      return *tx_hash;
    };

    {
      std::vector<cryptonote::tx_extra_field> extra;
      cryptonote::parse_tx_extra(tx.extra, extra);
      if (!cryptonote::find_tx_extra_field_by_type(extra, key) && !is_carrot)
        return;

      extra_nonce.emplace();
      if (cryptonote::find_tx_extra_field_by_type(extra, *extra_nonce))
      {
        if (cryptonote::get_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.long_))
          payment_id.first = sizeof(crypto::hash);
      }
      else
        extra_nonce = boost::none;

      if (subaddress || is_carrot)
        cryptonote::find_tx_extra_field_by_type(extra, additional_tx_pub_keys);
    }

    for (account& user : users)
    {
      if (height <= user.scan_height())
        continue;

      if (payment_id.first == sizeof(crypto::hash8))
        payment_id = {};

      const account::key_type account_type = user.type();
      const crypto::secret_key& view_key = user.view_key();

      mx25519_pubkey derived;
      crypto::key_derivation ed25519_derived;
      if (is_carrot && ::carrot::try_make_carrot_shared_key_receiver(view_key, ::carrot::raw_byte_convert<mx25519_pubkey>(key.pub_key), derived))
      {}
      else if (!is_carrot && crypto::wallet::generate_key_derivation(key.pub_key, view_key, ed25519_derived))
      {
        derived = ::carrot::raw_byte_convert<mx25519_pubkey>(ed25519_derived);
      }
      else // failed derivation
        continue;

      if (subaddress && additional_tx_pub_keys.data.size() == tx.vout.size())
      if ((is_carrot || subaddress) && additional_tx_pub_keys.data.size() == tx.vout.size())
      {
        additional_derivations.resize(tx.vout.size());
        for (std::size_t index = 0; index < tx.vout.size(); ++index)
        {
          if (is_carrot && ::carrot::try_make_carrot_shared_key_receiver(view_key, ::carrot::raw_byte_convert<mx25519_pubkey>(additional_tx_pub_keys.data[index]), additional_derivations[index]))
          {}
          else if (!is_carrot && crypto::wallet::generate_key_derivation(additional_tx_pub_keys.data[index], view_key, ed25519_derived))
          {
            additional_derivations[index] = ::carrot::raw_byte_convert<mx25519_pubkey>(ed25519_derived);
          }
          else // failed derivation
          {
            additional_derivations.clear();
            break;
          }
        }
      }

      db::extra ext{};
      std::uint32_t mixin = 0;
      std::size_t index = -1;
      crypto::key_image first_key_image{};
      cryptonote::txin_gen const* coinbase = nullptr;
      for (auto const& in : tx.vin)
      {
        ++index;

        if (const auto in_data = boost::get<cryptonote::txin_to_key>(std::addressof(in)))
        {
          mixin = boost::numeric_cast<std::uint32_t>(std::max<std::size_t>(1, in_data->key_offsets.size()) - 1);

          if (index == 0 && is_carrot)
            first_key_image = in_data->k_image;

          const auto carrot_subaccount =
            account_type == account::key_type::balance ?
              user.get_spendable(in_data->k_image) : std::nullopt;
          if (carrot_subaccount)
          {
            // ::carrot balance-key case: output key image was observed
            on_spend(
              user,
              db::spend{
                db::transaction_link{height, get_tx_hash()},
                in_data->k_image,
                carrot_subaccount->first,
                timestamp,
                tx.unlock_time,
                db::carrot_internal,
                {0, 0, 0}, // reserved
                0,
                crypto::hash{},
                carrot_subaccount->second
              }
            );
          }

          std::uint64_t goffset = 0;
          for (std::uint64_t offset : in_data->key_offsets)
          {  
            goffset += offset;
            const auto address_index = user.get_spendable(db::output_id{in_data->amount, goffset});
            if (!address_index)
              continue;

            // original spend case: output listed in ring
            on_spend(
              user,
              db::spend{
                db::transaction_link{height, get_tx_hash()},
                in_data->k_image,
                db::output_id{in_data->amount, goffset},
                timestamp,
                tx.unlock_time,
                mixin,
                {0, 0, 0},
                payment_id.first,
                payment_id.second.long_,
                *address_index
              }
            );
          }
        }
        else if ((coinbase = boost::get<cryptonote::txin_gen>(std::addressof(in))))
          ext = db::extra(ext | db::coinbase_output);
      }

      for (std::size_t index = 0; index < tx.vout.size(); ++index)
      {
        crypto::public_key out_pub_key;
        if (!cryptonote::get_output_public_key(tx.vout[index], out_pub_key))
          continue;

        const auto view_tag = cryptonote::get_output_view_tag(tx.vout[index]);
        const bool matched =
            is_carrot ||
            (!additional_derivations.empty() && cryptonote::out_can_be_to_acc(view_tag, ::carrot::raw_byte_convert<crypto::key_derivation>(additional_derivations.at(index)), index)) ||
            cryptonote::out_can_be_to_acc(view_tag, ::carrot::raw_byte_convert<crypto::key_derivation>(derived), index);

        if (!matched)
          continue;

        std::uint64_t amount = tx.vout.at(index).amount;
        ::carrot::CarrotEnoteType enote_type = ::carrot::CarrotEnoteType::PAYMENT;
        std::optional<db::address_index> account_index;
        mx25519_pubkey active_derivation{};
        crypto::public_key active_pub{};
        rct::key mask = rct::identity();
        ::carrot::encrypted_janus_anchor_t anchor{};

        for (std::size_t attempt = 0; attempt < 2; ++attempt)
        {
          if (attempt == 0)
          {
            active_derivation = derived;
            active_pub = key.pub_key;
          }
          else if (!additional_derivations.empty())
          {
            active_derivation = additional_derivations.at(index);
            active_pub = additional_tx_pub_keys.data.at(index);
          }
          else
            break;

          crypto::public_key derived_pub{};
          cryptonote::txout_to_carrot_v1 const* const carrot_info =
            boost::get<cryptonote::txout_to_carrot_v1>(std::addressof(tx.vout.at(index).target));
          if (is_carrot && carrot_info)
          {
            payment_id = {};
            mixin = db::carrot_external;
            anchor = carrot_info->encrypted_janus_anchor;

            if (coinbase)
            {
              if (::carrot::try_scan_carrot_coinbase_enote_receiver(
                ::carrot::CarrotCoinbaseEnoteV1{
                  out_pub_key,
                  tx.vout.at(index).amount,
                  anchor,
                  carrot_info->view_tag,
                  ::carrot::raw_byte_convert<mx25519_pubkey>(active_pub),
                  coinbase->height
                },
                active_derivation,
                {std::addressof(user.spend_public()), 1},
                is_carrot->gout,
                is_carrot->tout,
                derived_pub
              ))
              { account_index = is_match(derived_pub, user, subaddress, height); }
            }
            else if (index < tx.rct_signatures.outPk.size())
            {
              crypto::hash8 temp{};
              ::carrot::payment_id_t decrypted_id{};
              ::carrot::janus_anchor_t janus;
              std::optional<::carrot::encrypted_payment_id_t> cpayment_id;

              ::carrot::CarrotEnoteV1 enote{
                out_pub_key,
                ::carrot::raw_byte_convert<::carrot::amount_commitment_t>(tx.rct_signatures.outPk.at(index).mask),
                ::carrot::encrypted_amount_t{},
                anchor,
                carrot_info->view_tag,
                ::carrot::raw_byte_convert<mx25519_pubkey>(active_pub),
                first_key_image
              };

              static_assert(sizeof(enote.amount_enc) <= sizeof(tx.rct_signatures.ecdhInfo.at(index).amount));
              if (index < tx.rct_signatures.ecdhInfo.size())
                std::memcpy(std::addressof(enote.amount_enc), std::addressof(tx.rct_signatures.ecdhInfo.at(index).amount), sizeof(enote.amount_enc));

              if (extra_nonce && cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce->nonce, temp))
                cpayment_id = ::carrot::raw_byte_convert<::carrot::encrypted_payment_id_t>(temp);

              const ::carrot::view_incoming_key_ram_borrowed_device incoming_device{view_key};
              const ::carrot::view_balance_secret_ram_borrowed_device balance_device{user.balance_key()};

              if (::carrot::try_scan_carrot_enote_external_receiver(
                enote,
                cpayment_id,
                active_derivation,
                {std::addressof(user.spend_public()), 1},
                incoming_device,
                is_carrot->gout,
                is_carrot->tout,
                derived_pub,
                amount,
                is_carrot->blinding,
                decrypted_id,
                enote_type
                ) && (account_index = is_match(derived_pub, user, subaddress, height)))
              {
                payment_id.first = sizeof(crypto::hash8);
                payment_id.second.short_ = ::carrot::raw_byte_convert<crypto::hash8>(decrypted_id);
              }
              else if (account_type == account::key_type::balance && ::carrot::try_scan_carrot_enote_internal_receiver(
                enote,
                balance_device,
                is_carrot->gout,
                is_carrot->tout,
                derived_pub,
                amount,
                is_carrot->blinding,
                enote_type,
                janus
                ) && (account_index = is_match(derived_pub, user, subaddress, height)))
              {
                mixin = db::carrot_internal;
              }
              else
                continue; // to next available active_derivation
                mask = ::carrot::raw_byte_convert<rct::key>(tools::unwrap(is_carrot->blinding));
            }
            else
            {
              MWARNING("Invalid tx format detected: " << tx_hash);
              continue; // to next active_derivation
            }
          }
          else // !is_carrot
          {
            if (crypto::wallet::derive_subaddress_public_key(out_pub_key, ::carrot::raw_byte_convert<crypto::key_derivation>(active_derivation), index, derived_pub))
              account_index = is_match(derived_pub, user, subaddress, height);
          }
        }

        // check for `!is_carrot` or `is_carrot && coinbase`
        if (!account_index)
          continue;

        if (enote_type == ::carrot::CarrotEnoteType::CHANGE && account_type != account::key_type::balance)
        {
          // Detected spend via ::carrot protocol (change received)
          for (auto const& in : tx.vin)
          {
            cryptonote::txin_to_key const* const in_data =
              boost::get<cryptonote::txin_to_key>(std::addressof(in));
            if (in_data && in_data->key_offsets.empty())
            {
              on_spend(
                user,
                db::spend{
                  db::transaction_link{height, get_tx_hash()},
                  in_data->k_image,
                  db::output_id::unknown_spend(), // no clue which output was spent
                  timestamp,
                  tx.unlock_time,
                  db::carrot_external,
                  {0, 0, 0}, // reserved
                  0,
                  crypto::hash{},
                  db::address_index{account_index->maj_i, db::minor_index::primary}, // best guess
                }
              );
            }
          }
        }

        if (!prefix_hash)
        {
          prefix_hash.emplace();
          cryptonote::get_transaction_prefix_hash(tx, *prefix_hash);
        }

        if (!amount && !is_carrot && !(ext & db::coinbase_output) && 1 < tx.version)
        {
          const bool bulletproof2 = (rct::RCTTypeBulletproof2 <= tx.rct_signatures.type);
          const auto decrypted = lws::decode_amount(
            tx.rct_signatures.outPk.at(index).mask,
            tx.rct_signatures.ecdhInfo.at(index),
            ::carrot::raw_byte_convert<crypto::key_derivation>(active_derivation),
            index,
            bulletproof2
          );
          if (!decrypted)
          {
            MWARNING(user.address() << " failed to decrypt amount for tx " << get_tx_hash() << ", skipping output");
            continue;
          }
          amount = decrypted->first;
          mask = decrypted->second;
          ext = db::extra(ext | db::ringct_output);
        }
        else if (1 < tx.version)
          ext = db::extra(ext | db::ringct_output);

        if (!is_carrot && extra_nonce && !payment_id.first && cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.short_))
        {
          payment_id.first = sizeof(crypto::hash8);
          lws::decrypt_payment_id(payment_id.second.short_, ::carrot::raw_byte_convert<crypto::key_derivation>(active_derivation));
        }

        const std::uint64_t id = out_ids.at(index);
        on_output(
          user,
          db::output{
            db::transaction_link{height, get_tx_hash()},
            db::output::spend_meta_{
              is_unified ? db::output_id::unified(id) : db::output_id{tx.version < 2 ? tx.vout[index].amount : 0, id},
              amount,
              mixin,
              boost::numeric_cast<std::uint32_t>(index),
              active_pub
            },
            timestamp,
            tx.unlock_time,
            *prefix_hash,
            out_pub_key,
            mask,
            {0, 0, 0, 0, 0, 0, 0},
            db::pack(ext, payment_id.first),
            payment_id.second,
            cryptonote::get_tx_fee(tx),
            *account_index,
            first_key_image,
            anchor
          }
        );
      }
    }
  }

  subaddress_reader::subaddress_reader(db::storage const& disk_in, const std::uint32_t max_subaddresses)
    : reader(common_error::kInvalidArgument), disk(disk_in.clone()), cur(nullptr), max_subaddresses(max_subaddresses)
  {
    update_reader();
  }

  void subaddress_reader::update_reader()
  {
    reader = disk.start_read();
    if (!reader)
      MERROR("Subadress lookup failure: " << reader.error().message());
  }
  
  expect<db::address_index> subaddress_reader::find_subaddress(const account& user, crypto::public_key const& pubkey)
  {
    if (!reader)
      return {lmdb::error(MDB_NOTFOUND)};
    return reader->find_subaddress(user.id(), pubkey, cur);
  }

  expect<void> subaddress_reader::update_lookahead(const account& user, const db::address_index& match, db::block_id height)
  {
    if (match.is_zero())
      return {}; // keep subaddress disabled servers quick

    if (height == db::block_id::txpool)
      height = user.scan_height();

    auto upserted = disk.update_lookahead(user.db_address(), height, match, max_subaddresses);
    if (!upserted)
      return upserted.error();
    if (0 < *upserted)
      update_reader(); // update reader after upsert added new addresses
    else if (*upserted < 0)
      upserted = {error::max_subaddresses};
    return upserted.error();
  }

} // namespace lws
