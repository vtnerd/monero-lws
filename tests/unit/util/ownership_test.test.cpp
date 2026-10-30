// Copyright (c) 2026, The Monero Project
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

#include "framework.test.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "crypto/crypto.h"                            // monero/src
#include "cryptonote_basic/account.h"                 // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_basic/tx_extra.h"                // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h"      // monero/src
#include "db/account.h"
#include "db/data.h"
#include "util/account.test.h"
#include "util/ownership_test.h"
#include "util/transactions.h"

namespace
{
  constexpr const std::uint32_t ring_base = 20;

  lws::db::account make_account(const cryptonote::account_keys& src, const lws::db::account_id id)
  {
    lws::db::account out{};
    out.id = id;
    std::memcpy(&out.address.spend_public, &src.m_account_address.m_spend_public_key, sizeof(out.address.spend_public));
    std::memcpy(&out.address.view_public, &src.m_account_address.m_view_public_key, sizeof(out.address.view_public));
    std::memcpy(&out.key, &unwrap(unwrap(src.m_view_secret_key)), sizeof(out.key));
    return out;
  }

  //! Pays `receiver` with change to `sender`, same input setup as `lws_test::make_tx`.
  cryptonote::transaction make_tx(lest::env& lest_env, const cryptonote::account_keys& sender, const cryptonote::account_keys& receiver, const crypto::hash8& payment_id)
  {
    crypto::secret_key unused_key{};
    crypto::secret_key og_tx_key{};
    crypto::public_key og_tx_public{};
    crypto::generate_keys(og_tx_public, og_tx_key);

    crypto::key_derivation derivation{};
    crypto::public_key spend_public{};
    EXPECT(crypto::generate_key_derivation(sender.m_account_address.m_view_public_key, og_tx_key, derivation));
    EXPECT(crypto::derive_public_key(derivation, 0, sender.m_account_address.m_spend_public_key, spend_public));

    std::vector<cryptonote::tx_source_entry> sources;
    sources.emplace_back();
    sources.back().amount = 20000;
    sources.back().rct = true;
    sources.back().real_output = 15;
    sources.back().real_output_in_tx_index = 0;
    sources.back().real_out_tx_key = og_tx_public;
    for (std::uint32_t i = ring_base; i < 15 + ring_base; ++i)
    {
      crypto::public_key next{};
      crypto::generate_keys(next, unused_key);
      sources.back().push_output(i, next, 10000);
    }
    sources.back().outputs.emplace_back();
    sources.back().outputs.back().first = 15 + ring_base;
    sources.back().outputs.back().second.dest = rct::pk2rct(spend_public);

    std::vector<cryptonote::tx_destination_entry> destinations;
    destinations.emplace_back(8000, receiver.m_account_address, false);
    destinations.emplace_back(8000, sender.m_account_address, false); // change

    // construct_tx encrypts the payment id for the only non-change destination
    std::string nonce;
    cryptonote::set_encrypted_payment_id_to_tx_extra_nonce(nonce, payment_id);
    std::vector<std::uint8_t> extra;
    EXPECT(cryptonote::add_extra_nonce_to_tx_extra(extra, nonce));

    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    subaddresses[sender.m_account_address.m_spend_public_key] = {0, 0};

    cryptonote::transaction tx{};
    std::vector<crypto::secret_key> additional_keys;
    EXPECT(
      cryptonote::construct_tx_and_get_tx_key(
        sender, subaddresses, sources, destinations, sender.m_account_address, extra, tx, unused_key,
        additional_keys, true, {rct::RangeProofType::RangeProofPaddedBulletproof, 2}, true
      )
    );
    EXPECT(additional_keys.empty());
    return tx;
  }

  crypto::hash8 decrypt(lest::env& lest_env, const cryptonote::transaction& tx, const cryptonote::account_keys& keys)
  {
    std::vector<cryptonote::tx_extra_field> extra;
    EXPECT(cryptonote::parse_tx_extra(tx.extra, extra));

    cryptonote::tx_extra_pub_key key;
    EXPECT(cryptonote::find_tx_extra_field_by_type(extra, key));

    cryptonote::tx_extra_nonce nonce;
    EXPECT(cryptonote::find_tx_extra_field_by_type(extra, nonce));

    crypto::hash8 out{};
    EXPECT(cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(nonce.nonce, out));

    crypto::key_derivation derivation{};
    EXPECT(crypto::generate_key_derivation(key.pub_key, keys.m_view_secret_key, derivation));
    lws::decrypt_payment_id(out, derivation);
    return out;
  }
}

LWS_CASE("lws::ownership_test")
{
  const auto sender = lws_test::make_account();
  const auto receiver = lws_test::make_account();
  const lws::db::account_id sender_id{1};
  const lws::db::account_id receiver_id{2};

  SETUP("Encrypted payment id with sender and receiver both registered")
  {
    const crypto::hash8 real_id{{1, 2, 3, 4, 5, 6, 7, 8}};
    const cryptonote::transaction tx = make_tx(lest_env, sender, receiver, real_id);
    EXPECT(tx.vout.size() == 2);
    EXPECT(decrypt(lest_env, tx, receiver) == real_id);

    // The sender's key gives a different value, which is also what a full
    // wallet for the sender shows.
    const crypto::hash8 sender_decrypted = decrypt(lest_env, tx, sender);
    EXPECT(sender_decrypted != real_id);

    std::vector<std::pair<lws::db::account_id, lws::db::output>> outputs;
    std::vector<std::pair<lws::db::account_id, lws::db::spend>> spends;
    lws::ownership_test scan{
      [&spends] (lws::account& user, lws::db::spend&& spend)
      { spends.emplace_back(user.id(), std::move(spend)); },
      [&outputs] (lws::account& user, lws::db::output&& out, lws::db::storage_reader*)
      { outputs.emplace_back(user.id(), std::move(out)); }
    };

    const auto run = [&] (const bool sender_first)
    {
      // The sender owns the real ring member, so the input shows up as its spend.
      std::vector<std::pair<lws::db::output_id, lws::db::address_index>> spendable{
        {lws::db::output_id{0, 15 + ring_base}, lws::db::address_index{}}
      };

      std::vector<lws::account> users;
      if (sender_first)
      {
        users.emplace_back(make_account(sender, sender_id), std::move(spendable), std::vector<crypto::public_key>{});
        users.emplace_back(make_account(receiver, receiver_id), std::vector<std::pair<lws::db::output_id, lws::db::address_index>>{}, std::vector<crypto::public_key>{});
      }
      else
      {
        users.emplace_back(make_account(receiver, receiver_id), std::vector<std::pair<lws::db::output_id, lws::db::address_index>>{}, std::vector<crypto::public_key>{});
        users.emplace_back(make_account(sender, sender_id), std::move(spendable), std::vector<crypto::public_key>{});
      }

      scan(epee::to_mut_span(users), lws::db::block_id(10), 0, nullptr, tx, {100, 101});

      EXPECT(outputs.size() == 2);
      for (const auto& output : outputs)
      {
        EXPECT(lws::db::unpack(output.second.extra).second == sizeof(crypto::hash8));
        if (output.first == receiver_id)
          EXPECT(output.second.payment_id.short_ == real_id);
        else
        {
          EXPECT(output.first == sender_id);
          EXPECT(output.second.payment_id.short_ == sender_decrypted);
        }
      }

      // Spends only carry unencrypted payment ids (see `db::spend`).
      EXPECT(spends.size() == 1);
      EXPECT(spends.at(0).first == sender_id);
      EXPECT(spends.at(0).second.length == 0);
      EXPECT(spends.at(0).second.payment_id == crypto::hash{});
    };

    SECTION("sender scanned first")
    {
      run(true);
    }

    SECTION("receiver scanned first")
    {
      run(false);
    }
  }
}
