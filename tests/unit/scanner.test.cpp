// Copyright (c) 2023, The Monero Project
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

#include "scanner.test.h"
#include "framework.test.h"

#include <boost/thread.hpp>
#include <tuple>

#include "carrot_core/account_secrets.h"
#include "carrot_core/device_ram_borrowed.h"          // monero/src
#include "carrot_core/enote_utils.h"                  // monero/src
#include "carrot_impl/address_device_ram_borrowed.h"  // monero/src
#include "carrot_impl/format_utils.h"                 // monero/src
#include "carrot_impl/key_image_device_composed.h"    // monero/src
#include "carrot_impl/output_opening_types.h"         // monero/src
#include "carrot_impl/tx_builder_inputs.h"            // monero/src
#include "carrot_impl/tx_builder_outputs.h"           // monero/src
#include "carrot_impl/tx_proposal_utils.h"            // monero/src
#include "common/apply_permutation.h" // monero/src
#include "cryptonote_basic/account.h" // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "cryptonote_config.h"        // monero/src
#include "cryptonote_core/cryptonote_tx_utils.h"      // monero/src
#include "db/chain.test.h"
#include "db/print.test.h"
#include "db/storage.test.h"
#include "device/device_default.hpp" // monero/src
#include "fcmp_pp/curve_trees.h"     // monero/src
#include "fcmp_pp/prove.h"           // monero/src
#include "fcmp_pp/tree_cache.h"      // monero/src
#include "hardforks/hardforks.h"     // monero/src
#include "net/zmq.h"                 // monero/src
#include "rpc/client.h"
#include "rpc/daemon_messages.h"     // monero/src
#include "scanner.h"
#include "wallet/tx_builder.h"       // monero/src
#include "wire/error.h"
#include "wire/json/write.h"

namespace
{
  using tree_cache = fcmp_pp::curve_trees::TreeCacheV1;
  using curve_tree = fcmp_pp::curve_trees::CurveTreesV1;

  constexpr const std::chrono::seconds message_timeout{3};
  constexpr const std::uint64_t fee_weight = 80000;

  template<typename T>
  struct json_rpc_response
  {
    T result;
    std::uint32_t id = 0;
  };

  template<typename T>
  void write_bytes(wire::json_writer& dest, const json_rpc_response<T>& self)
  {
    wire::object(dest, WIRE_FIELD(id), WIRE_FIELD(result));
  }

  template<typename T>
  epee::byte_slice to_json_rpc(T message)
  {
    epee::byte_slice out{};
    const std::error_code err =
      wire::json::to_bytes(out, json_rpc_response<T>{std::move(message)});
    if (err)
      MONERO_THROW(err, "Failed to serialize json_rpc_response");
    return out;
  }

  template<typename T>
  epee::byte_slice daemon_response(const T& message)
  {
    rapidjson::Value id;
    id.SetInt(0);
    return cryptonote::rpc::FullMessage::getResponse(message, id);
  }

  struct join
  {
      boost::thread& thread;
      ~join() { thread.join(); }
  };

  struct transaction
  {
    cryptonote::transaction tx;
    std::vector<crypto::secret_key> additional_keys;
    std::vector<crypto::public_key> pub_keys;
    std::vector<crypto::public_key> spend_publics;
    std::vector<carrot::RCTOutputEnoteProposal> carrot;
  };

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

  transaction make_miner_tx(lest::env& lest_env, const lws::db::block_id height)
  {
    return make_miner_tx(lest_env, height, lws::db::account_address{}, true);
  }

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
    }
    else
    {
      for (const auto& this_key : out.additional_keys)
      {
        out.pub_keys.emplace_back();
        EXPECT(crypto::secret_key_to_public_key(this_key, out.pub_keys.back()));
      }
    }
    return out;
  }

  fcmp_pp::curve_trees::OutputPair make_output_pair(const lws::db::output& source, const bool mask = false)
  {
    return {
      source.pub,
      mask ? source.ringct_mask : rct::commit(source.spend_meta.amount, source.ringct_mask)
    };
  }

  fcmp_pp::curve_trees::OutputPair make_output_pair(const cryptonote::transaction& tx, const std::size_t index)
  {
    crypto::public_key pub{};
    cryptonote::get_output_public_key(tx.vout.at(index), pub);
    const bool is_miner = tx.vin.at(0).type() == typeid(cryptonote::txin_gen);
    const std::uint64_t amount = tx.vout.at(index).amount;
    return {
      pub, is_miner ? rct::commit(amount, rct::identity()) : tx.rct_signatures.outPk.at(index).mask
    };
  }

  carrot::CarrotSelectedInput make_carrot_input(const carrot::view_incoming_key_device& incoming, const lws::db::output& source, const carrot::AddressDeriveType type)
  {
    if (source.is_carrot())
    {
      mx25519_pubkey shared{};
      carrot::view_tag_t tag{};
      const bool is_coinbase = lws::db::unpack(source.extra).first & lws::db::coinbase_output; 
      const mx25519_pubkey de = carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public);
      const carrot::input_context_t context =
        is_coinbase ?
          carrot::make_carrot_input_context_coinbase(std::uint64_t(source.link.height)) :
          carrot::make_carrot_input_context(source.first_image);

      if (!incoming.view_key_scalar_mult_x25519(carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public), shared))
        throw std::runtime_error{"Failed to generate x25519"};

      carrot::make_carrot_view_tag(shared.data, context, source.pub, tag);

      std::optional<carrot::encrypted_payment_id_t> epid;
      if (lws::db::unpack(source.extra).second == 8)
      {
        crypto::hash ctx{};
        carrot::payment_id_t upid{};

        static_assert(sizeof(upid) == sizeof(source.payment_id.short_));
        std::memcpy(std::addressof(upid), std::addressof(source.payment_id.short_), sizeof(upid));
        carrot::make_carrot_sender_receiver_secret(shared.data, de, context, ctx);
        epid.emplace(carrot::encrypt_legacy_payment_id(upid, ctx, source.pub));
      }

      return {
        source.spend_meta.amount,
        carrot::CarrotOutputOpeningHintV2{
          source.pub,
          source.ringct_mask,
          source.anchor,
          tag,
          carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public),
          source.first_image,
          source.spend_meta.amount,
          epid,
          carrot::subaddress_index_extended{
            std::uint32_t(source.recipient.maj_i),
            std::uint32_t(source.recipient.min_i),
            type
          }
        }
      };
    }
    // else not carrot
    return {
      source.spend_meta.amount,
      carrot::LegacyOutputOpeningHintV1{
        source.pub,
        source.spend_meta.tx_public,
        carrot::subaddress_index{
          std::uint32_t(source.recipient.maj_i),
          std::uint32_t(source.recipient.min_i)
        },
        source.spend_meta.amount,
        rct::identity(),
        source.spend_meta.index
      }
    };
  }

  carrot::CarrotDestinationV1 to_carrot_dest(const lws::db::account_address& dest, const carrot::payment_id_t& pid = {})
  {
    return {dest.spend_public, dest.view_public, false, pid};
  }

  carrot::CarrotDestinationV1 to_carrot_dest(const cryptonote::account_keys& dest, const carrot::subaddress_index& sub = {})
  {
    crypto::secret_key incoming{};
    crypto::secret_key address{};
    carrot::CarrotDestinationV1 out{};

    carrot::make_carrot_generateaddress_secret(dest.m_view_secret_key, address);
    const carrot::generate_address_secret_ram_borrowed_device address_device{address};

    const auto& account = dest.m_account_address;
    carrot::make_carrot_subaddress_v1(
      account.m_spend_public_key, account.m_view_public_key, address_device, sub.major, sub.minor, out
    );

    return out;
  }

  transaction make_carrot_tx(
    lest::env& lest_env,
    const cryptonote::account_keys& keys,
    const std::vector<carrot::CarrotDestinationV1>& dests,
    const tree_cache& cache,
    const curve_tree& tree,
    const std::vector<lws::db::output>& sources,
    const carrot::AddressDeriveType type = carrot::AddressDeriveType::PreCarrot
  )
  {
    struct select_inputs
    {
      std::vector<carrot::CarrotSelectedInput> sources;

      void operator()(
        const boost::multiprecision::uint128_t&,
        const std::map<std::size_t, rct::xmr_amount>&,
        std::size_t,
        std::size_t,
        std::vector<carrot::CarrotSelectedInput>& out)
      {
        out = sources;
      }
    };

    const carrot::cryptonote_hierarchy_address_device_ram_borrowed cryptonote_device{
      keys.m_account_address.m_spend_public_key, keys.m_view_secret_key
    };

    crypto::secret_key prove_key{};
    crypto::secret_key image_key{};
    crypto::secret_key address_key{};
    crypto::secret_key incoming_key{};
    crypto::public_key view_address{};

    if (type == carrot::AddressDeriveType::Carrot)
    {
      carrot::make_carrot_provespend_key(keys.m_spend_secret_key, prove_key);
      carrot::make_carrot_generateimage_key(keys.m_view_secret_key, image_key);
      carrot::make_carrot_generateaddress_secret(keys.m_view_secret_key, address_key);
      carrot::make_carrot_viewincoming_key(keys.m_view_secret_key, incoming_key);
      EXPECT(crypto::secret_key_to_public_key(incoming_key, view_address));
    }

    const carrot::carrot_hierarchy_address_device_ram_borrowed carrot_device{
      keys.m_account_address.m_spend_public_key,
      keys.m_account_address.m_view_public_key,
      view_address,
      address_key
    };
    const carrot::view_incoming_key_ram_borrowed_device incoming_device{
      incoming_key
    };
    const carrot::view_balance_secret_ram_borrowed_device balance_device{
      keys.m_view_secret_key
    };

    const carrot::generate_address_secret_ram_borrowed_device address_device{
      address_key
    };

    carrot::cryptonote_hierarchy_address_device const* cryptonote_device_ptr = nullptr;
    carrot::carrot_hierarchy_address_device const* carrot_device_ptr = nullptr;
    carrot::view_incoming_key_device const* incoming_device_ptr = nullptr;
    carrot::view_balance_secret_device const* balance_device_ptr = nullptr;
    if (type == carrot::AddressDeriveType::PreCarrot)
    {
      cryptonote_device_ptr = std::addressof(cryptonote_device);
      incoming_device_ptr = cryptonote_device_ptr;
    }
    else
    {
      carrot_device_ptr = std::addressof(carrot_device);
      incoming_device_ptr = std::addressof(incoming_device);
      balance_device_ptr = std::addressof(balance_device);
    }

    std::vector<carrot::CarrotSelectedInput> sources_real;
    for (const auto& source : sources)
      sources_real.push_back(make_carrot_input(*incoming_device_ptr, source, type));

    rct::xmr_amount amount = 0;
    for (const auto& source : sources)
      amount += source.spend_meta.amount;
    amount /= (dests.size() + 1);

    std::vector<carrot::CarrotPaymentProposalV1> payments;
    for (const auto& dest : dests)
      payments.push_back({dest, amount, carrot::gen_janus_anchor()});

    carrot::CarrotTransactionProposalV1 proposal;
    carrot::make_carrot_transaction_proposal_v1_transfer(
      payments,
      {},
      fee_weight,
      {},
      select_inputs{sources_real},
      keys.m_account_address.m_spend_public_key,
      carrot::subaddress_index_extended{{}, type},
      {},
      {},
      proposal
    );

    crypto::hash tx_hash{};
    carrot::encrypted_payment_id_t enc_pid{};
    std::vector<crypto::key_image> images;
    std::vector<fcmp_pp::FcmpPpSalProof> proofs;
    std::vector<FcmpRerandomizedOutputCompressed> rerandomized;
    std::vector<carrot::RCTOutputEnoteProposal> enotes;
    {
      const carrot::generate_image_key_ram_borrowed_device generate_device{
        type == carrot::AddressDeriveType::PreCarrot ?
          keys.m_spend_secret_key : image_key
      };
      const carrot::hybrid_hierarchy_address_device_composed hybrid_device{
        cryptonote_device_ptr, carrot_device_ptr
      };
      const carrot::key_image_device_composed image_device{
        generate_device, hybrid_device, balance_device_ptr, incoming_device_ptr
      };

      EXPECT_NO_THROW(
        carrot::make_signable_tx_hash_from_proposal_v1(
          proposal, balance_device_ptr, incoming_device_ptr, image_device, tx_hash
        )
      );

      std::vector<crypto::public_key> one_times;
      std::vector<rct::key> commitments;
      std::vector<rct::key> input_blinding;

      for (const auto& input : proposal.input_proposals)
      {
        one_times.push_back(onetime_address_ref(input));
        commitments.push_back(amount_commitment_ref(input));

        rct::xmr_amount amount;
        EXPECT(
          carrot::try_scan_opening_hint_amount(
            input,
            {std::addressof(keys.m_account_address.m_spend_public_key), 1},
            incoming_device_ptr,
            balance_device_ptr,
            amount,
            input_blinding.emplace_back()
          )
        );
      }

      std::vector<std::size_t> order;
      EXPECT_NO_THROW(
        carrot::get_sorted_input_key_images_from_proposal_v1(
          proposal, image_device, images, std::addressof(order)
        )
      );

      EXPECT_NO_THROW(
        carrot:get_output_enote_proposals_from_proposal_v1(
          proposal, balance_device_ptr, incoming_device_ptr, images.at(0), enotes, enc_pid
        )
      );

      std::vector<rct::key> output_blinding;
      for (const auto& enote : enotes)
        output_blinding.push_back(rct::sk2rct(enote.amount_blinding_factor));

      EXPECT_NO_THROW(
        carrot::make_carrot_rerandomized_outputs_nonrefundable(
          one_times, commitments, input_blinding, output_blinding, rerandomized
        )
      );

      std::size_t i = -1;
      for (const auto& input : proposal.input_proposals)
      {
        ++i;
        crypto::key_image ignored;
        const bool is_legacy =
          std::get_if<carrot::LegacyOutputOpeningHintV1>(std::addressof(input));
        if (type == carrot::AddressDeriveType::PreCarrot)
        {
          EXPECT_NO_THROW(
            carrot::make_sal_proof_any_to_legacy_v1(
              tx_hash, rerandomized.at(i), input, keys.m_spend_secret_key, cryptonote_device, proofs.emplace_back(), ignored
            )
          );
        }
        else
        {
          EXPECT_NO_THROW(
            carrot::make_sal_proof_any_to_carrot_v1(
              tx_hash,
              rerandomized.at(i),
              input,
              prove_key,
              image_key,
              balance_device,
              incoming_device,
              address_device,
              proofs.emplace_back(),
              ignored
            )
          );
        }
      }

      tools::apply_permutation(order, rerandomized);
      tools::apply_permutation(order, proofs);
    }

    return transaction{
      .tx = tools::wallet::finalize_fcmps_and_range_proofs(
        images, rerandomized, proofs, enotes, enc_pid, proposal.fee, cache, tree
      ),
      .carrot = enotes
    };
  }

  transaction make_carrot_tx(lest::env& lest_env, const cryptonote::account_keys& keys, const tree_cache& cache, const curve_tree& tree, const std::vector<lws::db::output>& sources)
  { 
    struct pair
    {
      crypto::public_key pub;
      crypto::secret_key sec;

      pair()
        : pub{}, sec{}
      {
        crypto::generate_keys(pub, sec);
      }
    };

    pair spend{};
    pair view{};

    return make_carrot_tx(lest_env, keys, {to_carrot_dest(lws::db::account_address{spend.pub, view.pub})}, cache, tree, sources);
  }

  void scanner_thread(lws::scanner& scanner, void* ctx, const std::vector<epee::byte_slice>& reply)
  {
    struct stop_
    {
      lws::scanner& scanner;
      ~stop_() { scanner.shutdown(); }; 
    } stop{scanner};

    lws_test::rpc_thread(ctx, reply);
  }
} // anonymous

namespace lws_test
{
  void rpc_thread(void* ctx, const std::vector<epee::byte_slice>& reply)
  {
    try
    {
      net::zmq::socket server{};
      server.reset(zmq_socket(ctx, ZMQ_REP));
      if (!server || zmq_bind(server.get(), lws_test::rpc_rendevous))
      {
        std::cout << "Failed to create ZMQ server" << std::endl;
        return;
      }

      for (const epee::byte_slice& message : reply)
      {
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
          const auto request = net::zmq::receive(server.get(), ZMQ_DONTWAIT);
          if (request)
            break;

          if (request != net::zmq::make_error_code(EAGAIN))
          {
            std::cout << "Failed to retrieve message in fake ZMQ server: " << request.error().message() << std::endl;;
            return;
          }

          if (message_timeout <= std::chrono::steady_clock::now() - start)
          {
            std::cout << "Timeout in dummy RPC server" << std::endl;
            return;
          }
          boost::this_thread::sleep_for(boost::chrono::milliseconds{10});
        } // until error or received message

        const auto sent = net::zmq::send(message.clone(), server.get());
        if (!sent)
        {
          std::cout << "Failed to send dummy RPC message: " << sent.error().message() << std::endl;
          return;
        }
      } // foreach message
    }
    catch (const std::exception& e)
    {
      std::cout << "Unexpected exception in dummy RPC server: " << e.what() << std::endl;
    }
  }
}

LWS_CASE("lws::scanner::sync and lws::scanner::run (legacy keys)")
{
  static constexpr const std::uint64_t miner_reward = 35184372088830;

  cryptonote::account_keys keys{};
  crypto::generate_keys(keys.m_account_address.m_spend_public_key, keys.m_spend_secret_key);
  crypto::generate_keys(keys.m_account_address.m_view_public_key, keys.m_view_secret_key);

  const lws::db::account_address account{
    keys.m_account_address.m_view_public_key,
    keys.m_account_address.m_spend_public_key
  };

  cryptonote::account_keys keys_subaddr1{};
  cryptonote::account_keys keys_subaddr2{};
  {
    hw::core::device_default hw{};
    keys_subaddr1.m_account_address = hw.get_subaddress(keys, cryptonote::subaddress_index{0, 1});
    keys_subaddr2.m_account_address = hw.get_subaddress(keys, cryptonote::subaddress_index{0, 2});

    const auto sub1_secret = hw.get_subaddress_secret_key(keys.m_view_secret_key, cryptonote::subaddress_index{0, 1});
    const auto sub2_secret = hw.get_subaddress_secret_key(keys.m_view_secret_key, cryptonote::subaddress_index{0, 2});

    sc_add(to_bytes(keys_subaddr1.m_spend_secret_key), to_bytes(sub1_secret), to_bytes(keys.m_spend_secret_key));
    sc_add(to_bytes(keys_subaddr1.m_view_secret_key), to_bytes(keys_subaddr1.m_spend_secret_key), to_bytes(keys.m_view_secret_key));

    sc_add(to_bytes(keys_subaddr2.m_spend_secret_key), to_bytes(sub2_secret), to_bytes(keys.m_spend_secret_key));
    sc_add(to_bytes(keys_subaddr2.m_view_secret_key), to_bytes(keys_subaddr2.m_spend_secret_key), to_bytes(keys.m_view_secret_key));
  } 

  cryptonote::account_keys keys2{};
  crypto::generate_keys(keys2.m_account_address.m_spend_public_key, keys2.m_spend_secret_key);
  crypto::generate_keys(keys2.m_account_address.m_view_public_key, keys2.m_view_secret_key);

  const lws::db::account_address account2{
    keys2.m_account_address.m_view_public_key,
    keys2.m_account_address.m_spend_public_key
  };

  cryptonote::account_keys keys3{};
  crypto::generate_keys(keys3.m_account_address.m_spend_public_key, keys3.m_spend_secret_key);

  // carrot view-balance account
  lws::db::account_address account3{};
  {
    crypto::secret_key incoming_key{};
    crypto::secret_key prove_key{};
    crypto::secret_key image_key{};
    crypto::key_derivation view_public{};

    carrot::make_carrot_provespend_key(keys3.m_spend_secret_key, prove_key);
    carrot::make_carrot_viewbalance_secret(keys3.m_spend_secret_key, keys3.m_view_secret_key);
    carrot::make_carrot_generateimage_key(keys3.m_view_secret_key, image_key);
    carrot::make_carrot_spend_pubkey(image_key, prove_key, account3.spend_public); 

    carrot::make_carrot_viewincoming_key(keys3.m_view_secret_key, incoming_key);
    EXPECT(crypto::secret_key_to_public_key(incoming_key, account3.view_public));

    keys3.m_account_address.m_spend_public_key = account3.spend_public;

    EXPECT(crypto::generate_key_derivation(account3.spend_public, incoming_key, view_public));
    keys3.m_account_address.m_view_public_key = carrot::raw_byte_convert<crypto::public_key>(view_public);

    keys3.m_account_address.m_view_public_key =
      rct::rct2pk(rct::scalarmultKey(rct::pk2rct(account3.spend_public), rct::sk2rct(incoming_key)));
  }

  SETUP("lws::rpc::context, ZMQ_REP Server, and lws::db::storage")
  {
    auto rpc = 
      lws::rpc::context::make(lws_test::rpc_rendevous, {}, {}, {}, std::chrono::minutes{0}, false);
  
    lws::db::test::cleanup_db on_scope_exit{};
    lws::db::storage db = lws::db::test::get_fresh_db();
    const lws::db::block_info last_block =
      MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_last_block());

    const auto get_account = [&db, &account] () -> lws::db::account
    {
      return MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(account)).second;
    };

    SECTION("lws::scanner::sync Invalid Response")
    {
      const crypto::hash hashes[1] = {
        last_block.hash
      };

      std::vector<epee::byte_slice> messages{};
      messages.push_back(to_json_rpc(1));

      lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};

      boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
      const join on_scope_exit{server_thread};
      EXPECT(!scanner.sync(MONERO_UNWRAP(rpc.connect())));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, hashes);
    }

    SECTION("lws::scanner::sync Update")
    {
      std::vector<epee::byte_slice> messages{};
      std::vector<crypto::hash> hashes{
        last_block.hash,
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>(),
        crypto::rand<crypto::hash>()
      };

      cryptonote::rpc::GetHashesFast::Response message{};

      message.start_height = std::uint64_t(last_block.id);
      message.hashes = hashes;
      message.current_height = message.start_height + hashes.size() - 1;
      messages.push_back(daemon_response(message));

      message.start_height = message.current_height;
      message.hashes.front() = message.hashes.back();
      message.hashes.resize(1);
      messages.push_back(daemon_response(message));

      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, {hashes.data(), 1});
      {
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
        lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
      }

      SECTION("Fork Chain")
      {
        messages.clear();
        hashes[2] = crypto::rand<crypto::hash>();
        hashes[3] = crypto::rand<crypto::hash>();
        hashes[4] = crypto::rand<crypto::hash>();
        hashes[5] = crypto::rand<crypto::hash>();

        message.start_height = std::uint64_t(last_block.id);
        message.hashes = hashes;
        messages.push_back(daemon_response(message));

        message.start_height = message.current_height;
        message.hashes.front() = message.hashes.back();
        message.hashes.resize(1);
        messages.push_back(daemon_response(message));

        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
        lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
      }
    }

    SECTION("lws::scanner::run (with upsert)")
    {
      const lws::db::block_id legacy_block_id = add(last_block.id, 2);
      const lws::db::block_id carrot_block_id = add(legacy_block_id, 2);
      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
      }

      const auto tree = fcmp_pp::curve_trees::curve_trees_v1();
      tree_cache cache(tree); 
      cache.init(std::uint64_t(last_block.id), last_block.hash, 0, {}, {});
 
      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = keys.m_account_address;

      std::vector<epee::byte_slice> messages{};
      transaction tx = make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      const lws::db::output legacy_output{
        lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx.tx)},
        lws::db::output::spend_meta_{
          lws::db::output_id{0, 100}, miner_reward, 0, 0, tx.pub_keys.at(0)
        },
        0,
        0,
        cryptonote::get_transaction_prefix_hash(tx.tx),
        tx.spend_publics.at(0),
        rct::commit(miner_reward, rct::identity()),
        {},
        lws::db::pack(lws::db::extra(lws::db::ringct_output | lws::db::coinbase_output), 0),
        {},
        0, // fee
        lws::db::address_index{}
      };

      transaction tx2 = make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);
 
      transaction tx3 = make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx4 = make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      cryptonote::rpc::GetBlocksFast::Response bmessage{};
      bmessage.start_height = std::uint64_t(last_block.id) + 1;
      bmessage.current_height = bmessage.start_height + 7;
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = tx.tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx2.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx3.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx4.tx));
      bmessage.blocks.back().transactions.push_back(tx2.tx);
      bmessage.blocks.back().transactions.push_back(tx3.tx);
      bmessage.blocks.back().transactions.push_back(tx4.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(100);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(101);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(102);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(200);
      bmessage.output_indices.back().back().push_back(201);
      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());
      /*bmessage.blocks.back().block.miner_tx =
        make_miner_tx(lest_env, lws::db::block_id(std::uint64_t(last_block.id) + 1), {}, false).tx; */

      EXPECT(cache.register_output(make_output_pair(tx.tx, 0)));
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height,
          get_block_hash(bmessage.blocks.at(0).block),
          last_block.hash,
          {{bmessage.start_height + 1, {{100, 0, make_output_pair(tx.tx, 0)}}}}
        )
      ); 
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height + 1,
          get_block_hash(bmessage.blocks.at(1).block),
          get_block_hash(bmessage.blocks.at(0).block),
          {{bmessage.start_height + 2, {{250, 0, make_output_pair(bmessage.blocks.back().block.miner_tx, 0)}}}}
        )
      );

      // third block
      transaction tx5 = make_carrot_tx(lest_env, keys, {to_carrot_dest(account3)}, cache, *tree, {legacy_output});

      static constexpr const std::uint64_t carrot1_payment = miner_reward / 2;
      const auto carrot1_block_id = increment(legacy_block_id);
      const bool carrot1_index = tx5.carrot.at(0).amount != carrot1_payment;
      const std::uint64_t carrot1_change = tx5.carrot.at(!carrot1_index).amount;

      const lws::db::output carrot1_output{
        lws::db::transaction_link{carrot1_block_id, cryptonote::get_transaction_hash(tx5.tx)},
        lws::db::output::spend_meta_{
          lws::db::output_id{0, std::uint64_t(301 + carrot1_index)},
          carrot1_payment,
          lws::db::carrot_external,
          std::uint32_t(carrot1_index),
          carrot::raw_byte_convert<crypto::public_key>(
            tx5.carrot.at(carrot1_index).enote.enote_ephemeral_pubkey
          )
        },
        0,
        0,
        cryptonote::get_transaction_prefix_hash(tx5.tx),
        tx5.carrot.at(carrot1_index).enote.onetime_address,
        tx5.carrot.at(carrot1_index).enote.amount_commitment,
        {},
        lws::db::pack(lws::db::extra::ringct_output, 8),
        {},
        519840000, // fee
        lws::db::address_index{},
        tx5.carrot.at(carrot1_index).enote.tx_first_key_image,
        tx5.carrot.at(carrot1_index).enote.anchor_enc
      };
 
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = make_miner_tx(lest_env, last_block.id).tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx5.tx));
      bmessage.blocks.back().transactions.push_back(tx5.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(300);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(301);
      bmessage.output_indices.back().back().push_back(302);

      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());

      EXPECT(cache.register_output(make_output_pair(carrot1_output, true)));
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height + 2,
          get_block_hash(bmessage.blocks.at(2).block),
          get_block_hash(bmessage.blocks.at(1).block),
          {{bmessage.start_height + 3, {{std::uint64_t(301 + carrot1_index), 0, make_output_pair(tx5.tx, carrot1_index)}}}}
        )
      );
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height + 3,
          get_block_hash(bmessage.blocks.at(3).block),
          get_block_hash(bmessage.blocks.at(2).block),
          {{bmessage.start_height + 4, {{350, 0, make_output_pair(bmessage.blocks.back().block.miner_tx, 0)}}}}
        )
      );

      // fifth block
      transaction tx6 = make_carrot_tx(
        lest_env, keys3, {to_carrot_dest(account, carrot::payment_id_t{{5}})}, cache, *tree, {carrot1_output}, carrot::AddressDeriveType::Carrot
      );

      static constexpr const std::uint64_t carrot2_payment = carrot1_payment / 2;
      const auto carrot2_block_id = add(legacy_block_id, 3);
      const bool carrot2_index = tx6.carrot.at(0).amount != carrot2_payment;
      const std::uint64_t carrot2_change = tx6.carrot.at(!carrot2_index).amount;

      const lws::db::output carrot2_output{
        lws::db::transaction_link{carrot2_block_id, cryptonote::get_transaction_hash(tx6.tx)},
        lws::db::output::spend_meta_{
          lws::db::output_id{0, std::uint64_t(401 + carrot2_index)},
          carrot2_payment,
          lws::db::carrot_external,
          std::uint32_t(carrot2_index),
          carrot::raw_byte_convert<crypto::public_key>(
            tx6.carrot.at(carrot2_index).enote.enote_ephemeral_pubkey
          )
        },
        0,
        0,
        cryptonote::get_transaction_prefix_hash(tx6.tx),
        tx6.carrot.at(carrot2_index).enote.onetime_address,
        tx6.carrot.at(carrot2_index).enote.amount_commitment,
        {},
        lws::db::pack(lws::db::extra::ringct_output, 8),
        {crypto::hash8{{5}}},
        519840000, // fee
        lws::db::address_index{},
        tx6.carrot.at(carrot2_index).enote.tx_first_key_image,
        tx6.carrot.at(carrot2_index).enote.anchor_enc
      };

      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = make_miner_tx(lest_env, last_block.id).tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx6.tx));
      bmessage.blocks.back().transactions.push_back(tx6.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(400);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(401);
      bmessage.output_indices.back().back().push_back(402);

      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());

      EXPECT(cache.register_output(make_output_pair(carrot2_output, true)));
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height + 4,
          get_block_hash(bmessage.blocks.at(4).block),
          get_block_hash(bmessage.blocks.at(3).block),
          {{bmessage.start_height + 5, {{std::uint64_t(401 + carrot2_index), 0, make_output_pair(tx6.tx, carrot2_index)}}}}
        )
      );
      EXPECT_NO_THROW(
        cache.sync_block(
          bmessage.start_height + 5,
          get_block_hash(bmessage.blocks.at(5).block),
          get_block_hash(bmessage.blocks.at(4).block),
          {}
        )
      );

      // seventh block
      transaction tx7 = make_carrot_tx(
        lest_env, keys, {to_carrot_dest(account3), to_carrot_dest(keys3, {0, 2})}, cache, *tree, {carrot2_output}
      );

      static constexpr const std::uint64_t carrot3_payment = carrot2_payment / 3;
      const auto carrot3_block_id = add(carrot2_block_id, 2);
      const bool is_carrot3_0_change = tx7.carrot.at(0).amount != carrot3_payment;
      const bool is_carrot3_1_change = tx7.carrot.at(1).amount != carrot3_payment;
      const bool is_carrot3_2_change = tx7.carrot.at(2).amount != carrot3_payment;
      const std::uint8_t carrot3_index1 = is_carrot3_0_change;
      const std::uint8_t carrot3_index2 = 1 + std::uint8_t(is_carrot3_0_change) + is_carrot3_1_change;
      const std::uint8_t carrot3_change_index =
        std::uint8_t(is_carrot3_1_change) + std::uint8_t(is_carrot3_2_change) * 2;
      const std::uint64_t carrot3_change = tx7.carrot.at(carrot3_change_index).amount;

      EXPECT(carrot3_index1 <= 1);
      EXPECT(1 <= carrot3_index2);
      EXPECT(carrot3_index2 <= 2);
      EXPECT(carrot3_change_index <= 2);
      EXPECT(carrot3_change_index != carrot3_index1);
      EXPECT(carrot3_change_index != carrot3_index2);
      EXPECT(carrot3_index1 != carrot3_index2);

      const auto miner_tx2 = make_single_enote_carrot_coinbase_transaction_v1(
        carrot::CarrotDestinationV1{account3.spend_public, account3.view_public, false, {}},
        miner_reward,
        std::uint64_t(carrot3_block_id),
        {}
      );
 
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = miner_tx2;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx7.tx));
      bmessage.blocks.back().transactions.push_back(tx7.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(500);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(501);
      bmessage.output_indices.back().back().push_back(502);
      bmessage.output_indices.back().back().push_back(503);

      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());
 
      std::vector<crypto::hash> hashes{
        last_block.hash,
        cryptonote::get_block_hash(bmessage.blocks.front().block),
      };
      {
        cryptonote::rpc::GetHashesFast::Response hmessage{};

        hmessage.start_height = std::uint64_t(last_block.id);
        hmessage.hashes = hashes;
        hmessage.current_height = hmessage.start_height + hashes.size() - 1;
        messages.push_back(daemon_response(hmessage));

        hmessage.start_height = hmessage.current_height;
        hmessage.hashes.front() = hmessage.hashes.back();
        hmessage.hashes.resize(1);
        messages.push_back(daemon_response(hmessage));

        {
          lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
          boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
          const join on_scope_exit{server_thread};
          EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
          lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
        }
      }

      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(1).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(2).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(3).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(4).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(5).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(6).block));
      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.at(7).block));

      EXPECT(db.add_account(account, keys.m_view_secret_key));
      EXPECT(db.add_account(account2, keys2.m_view_secret_key));
      EXPECT(db.add_account(account3, keys3.m_view_secret_key, lws::db::view_balance_key));
      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
        for (std::uint32_t i = 1; i < 4; i += 2)
        {
          const auto result =
            db.upsert_subaddresses(lws::db::account_id(i), std::nullopt, indexes, 2);
          EXPECT(result);
          EXPECT(result->size() == 1);
          EXPECT(result->at(0).first == lws::db::major_index::primary);
          EXPECT(result->at(0).second.get_container().size() == 1);
          EXPECT(result->at(0).second.get_container().at(0).size() == 2);
          EXPECT(result->at(0).second.get_container().at(0).at(0) == lws::db::minor_index(1));
          EXPECT(result->at(0).second.get_container().at(0).at(1) == lws::db::minor_index(2));
        }

        EXPECT(!db.upsert_subaddresses(lws::db::account_id(4), std::nullopt, indexes, 2));
      } 

      messages.clear();
      messages.push_back(daemon_response(bmessage));
      bmessage.start_height = bmessage.current_height;
      bmessage.blocks.front() = bmessage.blocks.back();
      bmessage.blocks.resize(1);
      bmessage.output_indices.front() = bmessage.output_indices.back();
      bmessage.output_indices.resize(1);
      messages.push_back(daemon_response(bmessage));
      {
        static constexpr const lws::scanner_options opts{1, false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        scanner.run(std::move(rpc), 1, {}, {}, opts);
      }
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));

      EXPECT(get_account().scan_height == add(carrot3_block_id, 1));

      using expected_map = std::map<std::tuple<lws::db::output_id, std::uint32_t, std::uint32_t>, lws::db::output>; 
      auto reader = MONERO_UNWRAP(db.start_read());

      const auto verify_outputs = [&lest_env, &reader] (const lws::db::account_id id, const std::size_t count, const expected_map& expected)
      {
        auto outputs = MONERO_UNWRAP(reader.get_outputs(id));
        EXPECT(count <= expected.size());
        EXPECT(outputs.count() == count);

        std::set<lws::db::output_id> matched;
        auto output_it = outputs.make_iterator();
        for (auto output_it = outputs.make_iterator(); !output_it.is_end(); ++output_it)
        {
          auto real_output = *output_it;
          const auto expected_output =
            expected.find(std::make_tuple(real_output.spend_meta.id, std::uint32_t(real_output.recipient.maj_i), std::uint32_t(real_output.recipient.min_i)));
          EXPECT(expected_output != expected.end());
          EXPECT(matched.insert(real_output.spend_meta.id).second);

          EXPECT(real_output.link.height == expected_output->second.link.height);
          EXPECT(real_output.link.tx_hash == expected_output->second.link.tx_hash);
          EXPECT(real_output.spend_meta.id == expected_output->second.spend_meta.id);
          EXPECT(real_output.spend_meta.amount == expected_output->second.spend_meta.amount);
          EXPECT(real_output.spend_meta.mixin_count == expected_output->second.spend_meta.mixin_count);
          EXPECT(real_output.spend_meta.index == expected_output->second.spend_meta.index);
          EXPECT(real_output.tx_prefix_hash == expected_output->second.tx_prefix_hash);
          EXPECT(real_output.spend_meta.tx_public == expected_output->second.spend_meta.tx_public);
          EXPECT(real_output.pub == expected_output->second.pub);
          EXPECT(rct::commit(real_output.spend_meta.amount, real_output.ringct_mask) == expected_output->second.ringct_mask);
          EXPECT(real_output.extra == expected_output->second.extra);
          if (unpack(expected_output->second.extra).second == 8)
            EXPECT(real_output.payment_id.short_ == expected_output->second.payment_id.short_);
          EXPECT(real_output.fee == expected_output->second.fee);
          EXPECT(real_output.recipient == expected_output->second.recipient);
          EXPECT(real_output.first_image == expected_output->second.first_image);
          EXPECT(real_output.anchor == expected_output->second.anchor);
        }
      };

      {
        const expected_map expected_account1{
          {
            {lws::db::output_id{0, 100}, 0, 0}, legacy_output 
          },
          {
            {lws::db::output_id{0, 101}, 0, 0}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx2.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 101}, 8000, 15, 0, tx2.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx2.tx),
              tx2.spend_publics.at(0),
              tx2.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
	        {
            {lws::db::output_id{0, 102}, 0, 0}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx3.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 102}, 8000, 15, 0, tx3.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx3.tx),
              tx3.spend_publics.at(0),
              tx3.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 200}, 0, 0}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 8000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 201}, 0, 0}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 8000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 200}, 0, 1}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 2000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 201}, 0, 1}, lws::db::output{
              lws::db::transaction_link{legacy_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 2000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(301 + !carrot1_index)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot1_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(301 + !carrot1_index)},
                carrot1_change,
                lws::db::carrot_external,
                std::uint32_t(!carrot1_index),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx5.carrot.at(!carrot1_index).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.carrot.at(!carrot1_index).enote.onetime_address,
              tx5.carrot.at(!carrot1_index).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              519840000, // fee
              lws::db::address_index{},
              tx5.carrot.at(!carrot1_index).enote.tx_first_key_image,
              tx5.carrot.at(!carrot1_index).enote.anchor_enc
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(401 + carrot2_index)}, 0, 0}, carrot2_output
          },
          {
            {lws::db::output_id{0, std::uint64_t(501 + carrot3_change_index)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(tx7.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(501 + carrot3_change_index)},
                carrot3_change,
                lws::db::carrot_external,
                std::uint32_t(carrot3_change_index),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx7.carrot.at(carrot3_change_index).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx7.tx),
              tx7.carrot.at(carrot3_change_index).enote.onetime_address,
              tx7.carrot.at(carrot3_change_index).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              574400000, // fee
              lws::db::address_index{},
              tx7.carrot.at(carrot3_change_index).enote.tx_first_key_image,
              tx7.carrot.at(carrot3_change_index).enote.anchor_enc
            }
          }
        };

        verify_outputs(lws::db::account_id(1), 8, expected_account1);
      }
      { 
        const expected_map expected_account3{
          {
            {lws::db::output_id{0, std::uint64_t(301 + carrot1_index)}, 0, 0}, carrot1_output
          },
          {
            {lws::db::output_id{0, std::uint64_t(401 + !carrot2_index)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot2_block_id, cryptonote::get_transaction_hash(tx6.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(401 + !carrot2_index)},
                carrot2_change,
                lws::db::carrot_internal,
                std::uint32_t(!carrot2_index),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx6.carrot.at(!carrot2_index).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx6.tx),
              tx6.carrot.at(!carrot2_index).enote.onetime_address,
              tx6.carrot.at(!carrot2_index).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 0),
              {},
              519840000, // fee
              lws::db::address_index{},
              tx6.carrot.at(!carrot2_index).enote.tx_first_key_image,
              tx6.carrot.at(!carrot2_index).enote.anchor_enc
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(500)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(miner_tx2)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(500)},
                miner_reward,
                lws::db::carrot_external,
                std::uint32_t(0),
                get_tx_pub_key_from_extra(miner_tx2),
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(miner_tx2),
              boost::get<cryptonote::txout_to_carrot_v1>(miner_tx2.vout.at(0).target).key,
              rct::commit(miner_reward, rct::identity()),
              {},
              lws::db::pack(lws::db::extra(lws::db::ringct_output | lws::db::coinbase_output), 0),
              {},
              0, // fee
              lws::db::address_index{},
              crypto::key_image{},
              boost::get<cryptonote::txout_to_carrot_v1>(miner_tx2.vout.at(0).target).encrypted_janus_anchor
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(501 + carrot3_index1)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(tx7.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(501 + carrot3_index1)},
                carrot3_payment,
                lws::db::carrot_external,
                std::uint32_t(carrot3_index1),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx7.carrot.at(carrot3_index1).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx7.tx),
              tx7.carrot.at(carrot3_index1).enote.onetime_address,
              tx7.carrot.at(carrot3_index1).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              574400000, // fee
              lws::db::address_index{},
              tx7.carrot.at(carrot3_index1).enote.tx_first_key_image,
              tx7.carrot.at(carrot3_index1).enote.anchor_enc
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(501 + carrot3_index1)}, 0, 2}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(tx7.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(501 + carrot3_index1)},
                carrot3_payment,
                lws::db::carrot_external,
                std::uint32_t(carrot3_index1),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx7.carrot.at(carrot3_index1).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx7.tx),
              tx7.carrot.at(carrot3_index1).enote.onetime_address,
              tx7.carrot.at(carrot3_index1).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              574400000, // fee
              {lws::db::major_index(0), lws::db::minor_index(2)},
              tx7.carrot.at(carrot3_index1).enote.tx_first_key_image,
              tx7.carrot.at(carrot3_index1).enote.anchor_enc
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(501 + carrot3_index2)}, 0, 0}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(tx7.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(501 + carrot3_index2)},
                carrot3_payment,
                lws::db::carrot_external,
                std::uint32_t(carrot3_index2),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx7.carrot.at(carrot3_index2).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx7.tx),
              tx7.carrot.at(carrot3_index2).enote.onetime_address,
              tx7.carrot.at(carrot3_index2).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              574400000, // fee
              lws::db::address_index{},
              tx7.carrot.at(carrot3_index2).enote.tx_first_key_image,
              tx7.carrot.at(carrot3_index2).enote.anchor_enc
            }
          },
          {
            {lws::db::output_id{0, std::uint64_t(501 + carrot3_index2)}, 0, 2}, lws::db::output{
              lws::db::transaction_link{carrot3_block_id, cryptonote::get_transaction_hash(tx7.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(501 + carrot3_index2)},
                carrot3_payment,
                lws::db::carrot_external,
                std::uint32_t(carrot3_index2),
                carrot::raw_byte_convert<crypto::public_key>(
                  tx7.carrot.at(carrot3_index2).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx7.tx),
              tx7.carrot.at(carrot3_index2).enote.onetime_address,
              tx7.carrot.at(carrot3_index2).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              574400000, // fee
              {lws::db::major_index(0), lws::db::minor_index(2)},
              tx7.carrot.at(carrot3_index2).enote.tx_first_key_image,
              tx7.carrot.at(carrot3_index2).enote.anchor_enc
            }
          }
        };
        verify_outputs(lws::db::account_id(3), 5, expected_account3);
      }
    } //SECTION (lws::scanner::run (with upsert))

    SECTION("lws::scanner::run (with lookahead)")
    {
      std::vector<cryptonote::tx_destination_entry> destinations;
      destinations.emplace_back();
      destinations.back().amount = 8000;
      destinations.back().addr = keys.m_account_address;

      std::vector<epee::byte_slice> messages{};
      transaction tx = make_miner_tx(lest_env, last_block.id, account, false);
      EXPECT(tx.pub_keys.size() == 1);
      EXPECT(tx.spend_publics.size() == 1);

      transaction tx2 = make_tx(lest_env, keys, destinations, 20, true);
      EXPECT(tx2.pub_keys.size() == 1);
      EXPECT(tx2.spend_publics.size() == 1);

      transaction tx3 = make_tx(lest_env, keys, destinations, 86, false);
      EXPECT(tx3.pub_keys.size() == 1);
      EXPECT(tx3.spend_publics.size() == 1);

      destinations.emplace_back();
      destinations.back().amount = 2000;
      destinations.back().addr = keys_subaddr1.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx4 = make_tx(lest_env, keys, destinations, 50, false);
      EXPECT(tx4.pub_keys.size() == 1);
      EXPECT(tx4.spend_publics.size() == 2);

      destinations.emplace_back();
      destinations.back().amount = 1000;
      destinations.back().addr = keys_subaddr2.m_account_address;
      destinations.back().is_subaddress = true;

      transaction tx5 = make_tx(lest_env, keys, destinations, 146, true);
      EXPECT(tx5.pub_keys.size() == 1);
      EXPECT(tx5.spend_publics.size() == 2);

      cryptonote::rpc::GetBlocksFast::Response bmessage{};
      bmessage.start_height = std::uint64_t(last_block.id) + 1;
      bmessage.current_height = bmessage.start_height + 1;
      bmessage.blocks.emplace_back();
      bmessage.blocks.back().block.miner_tx = tx.tx;
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx2.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx3.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx4.tx));
      bmessage.blocks.back().block.tx_hashes.push_back(cryptonote::get_transaction_hash(tx5.tx));
      bmessage.blocks.back().transactions.push_back(tx2.tx);
      bmessage.blocks.back().transactions.push_back(tx3.tx);
      bmessage.blocks.back().transactions.push_back(tx4.tx);
      bmessage.blocks.back().transactions.push_back(tx5.tx);
      bmessage.output_indices.emplace_back();
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(100);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(101);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(102);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(200);
      bmessage.output_indices.back().back().push_back(201);
      bmessage.output_indices.back().emplace_back();
      bmessage.output_indices.back().back().push_back(300);
      bmessage.output_indices.back().back().push_back(301);
      bmessage.blocks.push_back(bmessage.blocks.back());
      bmessage.output_indices.push_back(bmessage.output_indices.back());

      std::vector<crypto::hash> hashes{
        last_block.hash,
        cryptonote::get_block_hash(bmessage.blocks.back().block),
      };
      {
        cryptonote::rpc::GetHashesFast::Response hmessage{};

        hmessage.start_height = std::uint64_t(last_block.id);
        hmessage.hashes = hashes;
        hmessage.current_height = hmessage.start_height + hashes.size() - 1;
        messages.push_back(daemon_response(hmessage));

        hmessage.start_height = hmessage.current_height;
        hmessage.hashes.front() = hmessage.hashes.back();
        hmessage.hashes.resize(1);
        messages.push_back(daemon_response(hmessage));

        {
          lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
          boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
          const join on_scope_exit{server_thread};
          EXPECT(scanner.sync(MONERO_UNWRAP(rpc.connect())));
          lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));
        }
      }

      EXPECT(db.add_account(account, keys.m_view_secret_key));
      EXPECT(db.add_account(account2, keys2.m_view_secret_key));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{};
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      const lws::db::block_id user_height =
        MONERO_UNWRAP(MONERO_UNWRAP(db.start_read()).get_account(lws::db::account_status::active, lws::db::account_id(1))).scan_height;

      EXPECT(db.import_request(account, user_height, {lws::db::major_index(1), lws::db::minor_index(2)}));
      EXPECT(db.accept_requests(lws::db::request::import_scan, {std::addressof(account), 1}, 2));

      {
        auto reader = MONERO_UNWRAP(db.start_read());
        const std::vector<lws::db::subaddress_dict> expected_range{
          {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(1)}}}}
        };
        EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
      }

      messages.clear();
      messages.push_back(daemon_response(bmessage));
      bmessage.start_height = bmessage.current_height;
      bmessage.blocks.resize(1);
      bmessage.output_indices.resize(1);
      messages.push_back(daemon_response(bmessage));
      {
        static constexpr const lws::scanner_options opts{10, false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        scanner.run(std::move(rpc), 1, {}, {}, opts);
      }

      hashes.push_back(cryptonote::get_block_hash(bmessage.blocks.back().block));
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));

      const lws::db::block_id new_last_block_id = lws::db::block_id(std::uint64_t(last_block.id) + 2);
      EXPECT(get_account().scan_height == new_last_block_id);
      {
        const std::map<std::pair<lws::db::output_id, std::uint32_t>, lws::db::output> expected{
          {
            {lws::db::output_id{0, 100}, 35184372088830}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 100}, 35184372088830, 0, 0, tx.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx.tx),
              tx.spend_publics.at(0),
              rct::commit(35184372088830, rct::identity()),
              {},
              lws::db::pack(lws::db::extra(lws::db::extra::coinbase_output | lws::db::extra::ringct_output), 0),
              {},
              0, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 101}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx2.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 101}, 8000, 15, 0, tx2.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx2.tx),
              tx2.spend_publics.at(0),
              tx2.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
	        {
            {lws::db::output_id{0, 102}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx3.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 102}, 8000, 15, 0, tx3.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx3.tx),
              tx3.spend_publics.at(0),
              tx3.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              12000, // fee
              lws::db::address_index{}
            },
          },
          {
            {lws::db::output_id{0, 200}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 8000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 201}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 8000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 200}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 200}, 2000, 15, 0, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(0),
              tx4.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 201}, 2000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx4.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 201}, 2000, 15, 1, tx4.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx4.tx),
              tx4.spend_publics.at(1),
              tx4.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              10000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(1)}
            }
          },
          {
            {lws::db::output_id{0, 300}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 300}, 8000, 15, 0, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(0),
              tx5.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 301}, 8000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 301}, 8000, 15, 1, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(1),
              tx5.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{}
            }
          },
          {
            {lws::db::output_id{0, 300}, 1000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 300}, 1000, 15, 0, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(0),
              tx5.tx.rct_signatures.outPk.at(0).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(2)}
            }
          },
          {
            {lws::db::output_id{0, 301}, 1000}, lws::db::output{
              lws::db::transaction_link{new_last_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, 301}, 1000, 15, 1, tx5.pub_keys.at(0)
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.spend_publics.at(1),
              tx5.tx.rct_signatures.outPk.at(1).mask,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              11000, // fee
              lws::db::address_index{lws::db::major_index::primary, lws::db::minor_index(2)}
            }
          }
        };
        auto reader = MONERO_UNWRAP(db.start_read());
        auto outputs = MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(1)));
        EXPECT(outputs.count() == 6);
        auto output_it = outputs.make_iterator();
        for (auto output_it = outputs.make_iterator(); !output_it.is_end(); ++output_it)
        {
          auto real_output = *output_it;
          const auto expected_output =
            expected.find(std::make_pair(real_output.spend_meta.id, real_output.spend_meta.amount));
          EXPECT(expected_output != expected.end());

          EXPECT(real_output.link.height == expected_output->second.link.height);
          EXPECT(real_output.link.tx_hash == expected_output->second.link.tx_hash);
          EXPECT(real_output.spend_meta.id == expected_output->second.spend_meta.id);
          EXPECT(real_output.spend_meta.amount == expected_output->second.spend_meta.amount);
          EXPECT(real_output.spend_meta.mixin_count == expected_output->second.spend_meta.mixin_count);
          EXPECT(real_output.spend_meta.index == expected_output->second.spend_meta.index);
          EXPECT(real_output.tx_prefix_hash == expected_output->second.tx_prefix_hash);
          EXPECT(real_output.spend_meta.tx_public == expected_output->second.spend_meta.tx_public);
          EXPECT(real_output.pub == expected_output->second.pub);
          EXPECT(rct::commit(real_output.spend_meta.amount, real_output.ringct_mask) == expected_output->second.ringct_mask);
          EXPECT(real_output.extra == expected_output->second.extra);
          if (unpack(expected_output->second.extra).second == 8)
            EXPECT(real_output.payment_id.short_ == expected_output->second.payment_id.short_);
          EXPECT(real_output.fee == expected_output->second.fee);
          EXPECT(real_output.recipient == expected_output->second.recipient);
        }

        auto spends = MONERO_UNWRAP(reader.get_spends(lws::db::account_id(1)));
        EXPECT(spends.count() == 2);
        auto spend_it = spends.make_iterator();
        EXPECT(!spend_it.is_end());

        auto real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        lws::db::output_id expected_out{0, 100};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        ++spend_it;
        EXPECT(!spend_it.is_end());

        real_spend = *spend_it;
        EXPECT(real_spend.link.height == new_last_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        expected_out = lws::db::output_id{0, 101};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        EXPECT(MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(2))).count() == 0);
        EXPECT(MONERO_UNWRAP(reader.get_spends(lws::db::account_id(2))).count() == 0);

        {
          const std::vector<lws::db::subaddress_dict> expected_range{
            {lws::db::major_index(0), {{lws::db::index_range{lws::db::minor_index(0), lws::db::minor_index(2)}}}}
          };
          EXPECT(MONERO_UNWRAP(reader.get_subaddresses(lws::db::account_id(1))) == expected_range);
        }
      }
    } //SECTION (lws::scanner::run (lookahead))
  } // SETUP
} // LWS_CASE

