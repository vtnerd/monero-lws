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

#include "common/error.h"
#include "crypto/wallet/crypto.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
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
  ownership_test::ownership_test(spend_action on_spend, output_action on_output)
    : on_spend(on_spend)
    , on_output(on_output)
  {}

  void ownership_test::enable_subaddresses(const db::storage& disk, const std::uint32_t max_subaddresses)
  {
    subaddress.emplace(subaddress_reader{disk, max_subaddresses});
  }

  void ownership_test::disable_subaddresses()
  {
    subaddress.reset();
  }

  void ownership_test::operator()(
    epee::span<account> users,
    db::block_id height,
    std::uint64_t timestamp,
    const crypto::hash& tx_hash,
    const cryptonote::transaction& tx,
    const std::vector<std::uint64_t>& out_ids)
  {
    if (2 < tx.version)
      throw std::runtime_error{"Unsupported tx version"};

    cryptonote::tx_extra_pub_key key;
    boost::optional<crypto::hash> prefix_hash;
    boost::optional<cryptonote::tx_extra_nonce> extra_nonce;
    std::pair<std::uint8_t, db::output::payment_id_> payment_id;
    cryptonote::tx_extra_additional_pub_keys additional_tx_pub_keys;
    std::vector<crypto::key_derivation> additional_derivations;

    {
      std::vector<cryptonote::tx_extra_field> extra;
      cryptonote::parse_tx_extra(tx.extra, extra);
      if (!cryptonote::find_tx_extra_field_by_type(extra, key))
        return;

      extra_nonce.emplace();
      if (cryptonote::find_tx_extra_field_by_type(extra, *extra_nonce))
      {
        if (cryptonote::get_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.long_))
          payment_id.first = sizeof(crypto::hash);
      }
      else
        extra_nonce = boost::none;

      if (subaddress)
        cryptonote::find_tx_extra_field_by_type(extra, additional_tx_pub_keys);
    }

    for (account& user : users)
    {
      if (height <= user.scan_height())
        continue;

      crypto::key_derivation derived;
      if (!crypto::wallet::generate_key_derivation(key.pub_key, user.view_key(), derived))
        continue;

      if (subaddress && additional_tx_pub_keys.data.size() == tx.vout.size())
      {
        additional_derivations.resize(tx.vout.size());
        for (std::size_t index = 0; index < tx.vout.size(); ++index)
        {
          if (!crypto::wallet::generate_key_derivation(additional_tx_pub_keys.data[index], user.view_key(), additional_derivations[index]))
          {
            additional_derivations.clear();
            break;
          }
        }
      }

      db::extra ext{};
      std::uint32_t mixin = 0;
      for (auto const& in : tx.vin)
      {
        if (const auto in_data = boost::get<cryptonote::txin_to_key>(std::addressof(in)))
        {
          mixin = boost::numeric_cast<std::uint32_t>(std::max<std::size_t>(1, in_data->key_offsets.size()) - 1);

          std::uint64_t goffset = 0;
          for (std::uint64_t offset : in_data->key_offsets)
          {
            goffset += offset;
            const auto address_index = user.get_spendable(db::output_id{in_data->amount, goffset});
            if (!address_index)
              continue;

            on_spend(
              user,
              db::spend{
                db::transaction_link{height, tx_hash},
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
        else if (boost::get<cryptonote::txin_gen>(std::addressof(in)))
          ext = db::extra(ext | db::coinbase_output);
      }

      for (std::size_t index = 0; index < tx.vout.size(); ++index)
      {
        crypto::public_key out_pub_key;
        if (!cryptonote::get_output_public_key(tx.vout[index], out_pub_key))
          continue;

        const auto view_tag = cryptonote::get_output_view_tag(tx.vout[index]);
        const bool matched =
          (!additional_derivations.empty() && cryptonote::out_can_be_to_acc(view_tag, additional_derivations.at(index), index)) ||
          cryptonote::out_can_be_to_acc(view_tag, derived, index);

        if (!matched)
          continue;

        bool found_pub = false;
        db::address_index account_index{db::major_index::primary, db::minor_index::primary};
        crypto::key_derivation active_derivation{};
        crypto::public_key active_pub{};

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

          crypto::public_key derived_pub;
          if (!crypto::wallet::derive_subaddress_public_key(out_pub_key, active_derivation, index, derived_pub))
            continue;

          if (user.spend_public() != derived_pub)
          {
            if (!subaddress)
              continue;

            const expect<db::address_index> match = subaddress->find_subaddress(user, derived_pub);
            if (!match)
            {
              if (match != lmdb::error(MDB_NOTFOUND))
                MERROR("Failure when doing subaddress search: " << match.error().message());
              continue;
            }

            auto result = subaddress->update_lookahead(user, *match, height);
            if (!result)
              MWARNING("Failed to update lookahead for " << user.address() << ": " << result.error());
            found_pub = true;
            account_index = *match;
            break;
          }
          else
          {
            found_pub = true;
            break;
          }
        }

        if (!found_pub)
          continue;

        if (!prefix_hash)
        {
          prefix_hash.emplace();
          cryptonote::get_transaction_prefix_hash(tx, *prefix_hash);
        }

        std::uint64_t amount = tx.vout[index].amount;
        rct::key mask = rct::identity();
        if (!amount && !(ext & db::coinbase_output) && 1 < tx.version)
        {
          const bool bulletproof2 = (rct::RCTTypeBulletproof2 <= tx.rct_signatures.type);
          const auto decrypted = lws::decode_amount(
            tx.rct_signatures.outPk.at(index).mask,
            tx.rct_signatures.ecdhInfo.at(index),
            active_derivation,
            index,
            bulletproof2
          );
          if (!decrypted)
          {
            MWARNING(user.address() << " failed to decrypt amount for tx " << tx_hash << ", skipping output");
            continue;
          }
          amount = decrypted->first;
          mask = decrypted->second;
          ext = db::extra(ext | db::ringct_output);
        }
        else if (1 < tx.version)
          ext = db::extra(ext | db::ringct_output);

        if (extra_nonce && !payment_id.first && cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.short_))
        {
          payment_id.first = sizeof(crypto::hash8);
          lws::decrypt_payment_id(payment_id.second.short_, active_derivation);
        }

        on_output(
          user,
          db::output{
            db::transaction_link{height, tx_hash},
            db::output::spend_meta_{
              db::output_id{tx.version < 2 ? tx.vout[index].amount : 0, out_ids.at(index)},
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
            account_index
          }
        );
      }
    }
  }

  subaddress_reader::subaddress_reader(db::storage const& disk_in, const std::uint32_t max_subaddresses)
    : reader(common_error::kInvalidArgument), disk(), cur(nullptr), max_subaddresses(max_subaddresses)
  {
    disk = disk_in.clone();
    update_reader();
  }

  void subaddress_reader::update_reader()
  {
    reader = disk->start_read();
    if (!reader)
      MERROR("Subadress lookup failure: " << reader.error().message());
  }
  
  expect<db::address_index> subaddress_reader::find_subaddress(const account& user, crypto::public_key const& pubkey)
  {
    if (!reader)
      return {lmdb::error(MDB_NOTFOUND)};
    return reader->find_subaddress(user.id(), pubkey, cur);
  }

  expect<void> subaddress_reader::update_lookahead(const account& user, const db::address_index& match, const db::block_id height)
  {
    if (match.is_zero())
      return {}; // keep subaddress disabled servers quick

    auto upserted = disk->update_lookahead(user.db_address(), height, match, max_subaddresses);
    if (!upserted)
      return upserted.error();
    if (0 < *upserted)
      update_reader(); // update reader after upsert added new addresses
    else if (*upserted < 0)
      upserted = {error::max_subaddresses};
    return {};
  }

} // namespace lws
