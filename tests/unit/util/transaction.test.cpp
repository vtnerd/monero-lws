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

#include "transaction.test.h"

#include "carrot_core/payment_proposal.h"        // monero/src
#include "crypto/crypto.h"                       // monero/src
#include "cryptonote_basic/account.h"            // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h" // monero/src
#include "db/data.h"
#include "hardforks/hardforks.h"                 // monero/src
#include "util/transactions.h"

namespace lws_test
{
  transaction make_miner_tx(lest::env& lest_env, lws::db::block_id height, const lws::db::account_address& miner_address, bool use_view_tags)
  {
    static constexpr std::uint64_t fee = 0;

    transaction tx{};
    tx.pub_keys.emplace_back();
    tx.spend_publics.emplace_back();

    crypto::secret_key key;
    crypto::generate_keys(tx.pub_keys.back(), key);
    EXPECT(add_tx_pub_key_to_extra(tx.tx, tx.pub_keys.back()));

    cryptonote::txin_gen in;
    in.height = std::uint64_t(height);
    tx.tx.vin.push_back(in);

    // This will work, until size of constructed block is less then CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE
    uint64_t block_reward;
    EXPECT(cryptonote::get_block_reward(0, 0, 1000000, block_reward, num_testnet_hard_forks));
    block_reward += fee;

    crypto::key_derivation derivation;
    EXPECT(crypto::generate_key_derivation(miner_address.view_public, key, derivation));
    EXPECT(crypto::derive_public_key(derivation, 0, miner_address.spend_public, tx.spend_publics.back()));
 
    crypto::view_tag view_tag;
    if (use_view_tags)
      crypto::derive_view_tag(derivation, 0, view_tag);

    cryptonote::tx_out out;
    cryptonote::set_tx_out(block_reward, tx.spend_publics.back(), use_view_tags, view_tag, out);

    tx.tx.vout.push_back(out);
    tx.tx.version = 2;
    tx.tx.unlock_time = std::uint64_t(height) + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;

    return tx;
  }

  struct get_key_image
  {
    std::vector<crypto::key_image>& images;

    template<typename T>
    void operator()(const T&) const noexcept
    {}

    void operator()(const cryptonote::txin_to_key& val) const
    {
      images.push_back(val.k_image);
    }
  };

  struct get_spend_public
  {
    std::vector<crypto::public_key>& pub_keys;

    template<typename T>
    void operator()(const T&) const noexcept
    {}
   
    void operator()(const cryptonote::txout_to_key& val) const
    { pub_keys.push_back(val.key); }

    void operator()(const cryptonote::txout_to_tagged_key& val) const
    { pub_keys.push_back(val.key); }
  };

  transaction make_tx(lest::env& lest_env, const cryptonote::account_keys& keys, std::vector<cryptonote::tx_destination_entry> destinations, const std::uint32_t ring_base, const bool use_view_tag)
  {
    static constexpr std::uint64_t input_amount = 20000;
    static constexpr std::uint64_t output_amount = 8000;

    EXPECT(15 < std::numeric_limits<std::uint32_t>::max() - ring_base);

    crypto::secret_key unused_key{};
    crypto::secret_key og_tx_key{};
    crypto::public_key og_tx_public{};
    crypto::generate_keys(og_tx_public, og_tx_key);

    crypto::key_derivation derivation{};
    crypto::public_key spend_public{};
    EXPECT(crypto::generate_key_derivation(keys.m_account_address.m_view_public_key, og_tx_key, derivation));
    EXPECT(crypto::derive_public_key(derivation, 0, keys.m_account_address.m_spend_public_key, spend_public));

    std::uint32_t index = -1;
    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    for (const auto& destination : destinations)
    {
      ++index;
      subaddresses[destination.addr.m_spend_public_key] = {0, index};
    }

    if (2 < destinations.size())
      destinations.erase(destinations.begin() + 1, destinations.end() - 1);

    std::vector<cryptonote::tx_source_entry> sources;
    sources.emplace_back();
    sources.back().amount = input_amount;
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
 
    transaction out{};
    EXPECT(
      cryptonote::construct_tx_and_get_tx_key(
        keys, subaddresses, sources, destinations, keys.m_account_address, {}, out.tx, /* 0, */ unused_key,
        out.additional_keys, true, {rct::RangeProofType::RangeProofPaddedBulletproof, 2}, use_view_tag
      )
    );

    for (const auto& vin : out.tx.vin)
      boost::apply_visitor(get_key_image{out.images}, vin);

    for (const auto& vout : out.tx.vout)
      boost::apply_visitor(get_spend_public{out.spend_publics}, vout.target);

    if (out.additional_keys.empty())
    {
      std::vector<cryptonote::tx_extra_field> extra;
      EXPECT(cryptonote::parse_tx_extra(out.tx.extra, extra));
 
      cryptonote::tx_extra_pub_key key;
      EXPECT(cryptonote::find_tx_extra_field_by_type(extra, key));

      out.pub_keys.emplace_back();
      out.pub_keys.back() = key.pub_key;

      crypto::key_derivation derivation{};
      EXPECT(crypto::generate_key_derivation(key.pub_key, keys.m_view_secret_key, derivation));

      for (index = 0; index < out.tx.vout.size(); ++index)
      {
        const auto decrypted = lws::decode_amount(
          out.tx.rct_signatures.outPk.at(index).mask,
          out.tx.rct_signatures.ecdhInfo.at(index),
          derivation,
          index,
          true
        );
        EXPECT(bool(decrypted));
        out.ringct.push_back(decrypted->second);
      }
    }
    else
    {
      index = -1;
      for (const auto& this_key : out.additional_keys)
      {
        ++index;
        out.pub_keys.emplace_back();
        EXPECT(crypto::secret_key_to_public_key(this_key, out.pub_keys.back()));

        crypto::key_derivation derivation{};
        EXPECT(crypto::generate_key_derivation(out.pub_keys.back(), keys.m_view_secret_key, derivation));

        const auto decrypted = lws::decode_amount(
          out.tx.rct_signatures.outPk.at(index).mask,
          out.tx.rct_signatures.ecdhInfo.at(index),
          derivation,
          index,
          true
        );
        EXPECT(bool(decrypted));
        out.ringct.push_back(decrypted->second);
      }
    }

    for (const auto& rct : out.tx.rct_signatures.ecdhInfo)
      out.ringct.push_back(rct.mask);

    return out;
  }
} // lws_test
