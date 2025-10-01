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

#include "carrot_core/account_secrets.h"
#include "carrot_core/device_ram_borrowed.h"          // monero/src
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

  transaction make_tx(lest::env& lest_env, const cryptonote::account_keys& keys, std::vector<cryptonote::tx_destination_entry>& destinations, const std::uint32_t ring_base, const bool use_view_tag)
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

  fcmp_pp::curve_trees::OutputPair make_output_pair(const lws::db::output& source)
  {
    return {
      source.pub, rct::commit(source.spend_meta.amount, source.ringct_mask)
    };
  }

  carrot::CarrotSelectedInput make_carrot_input(const lws::db::output& source)
  {
    if (source.is_carrot())
    {
      return {
        source.spend_meta.amount,
        carrot::CarrotOutputOpeningHintV2{
          source.pub,
          rct::commit(source.spend_meta.amount, source.ringct_mask),
          {},
          {},
          carrot::raw_byte_convert<mx25519_pubkey>(source.spend_meta.tx_public),
          source.first_image,
          source.spend_meta.amount,
          std::nullopt,
          carrot::subaddress_index_extended{
            std::uint32_t(source.recipient.maj_i),
            std::uint32_t(source.recipient.min_i),
            carrot::AddressDeriveType::PreCarrot
          }
        }
      };
    }
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
        source.ringct_mask,
        source.spend_meta.index
      }
    };
  }

  transaction make_carrot_tx(lest::env& lest_env, const cryptonote::account_keys& keys, const lws::db::account_address& dest, const tree_cache& cache, const curve_tree& tree, const std::vector<lws::db::output>& sources, const carrot::payment_id_t& pid = {})
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
  
    std::vector<carrot::CarrotSelectedInput> sources_real;
    for (const auto& source : sources)
      sources_real.push_back(make_carrot_input(source));

    rct::xmr_amount amount = 0;
    for (const auto& source : sources)
      amount += source.spend_meta.amount;
    amount /= 2;

    carrot::CarrotTransactionProposalV1 proposal;
    carrot::make_carrot_transaction_proposal_v1_transfer(
      {
        carrot::CarrotPaymentProposalV1{
          carrot::CarrotDestinationV1{dest.spend_public, dest.view_public, false, pid},
          amount,
          carrot::gen_janus_anchor()
        }
      },
      {},
      fee_weight,
      {},
      select_inputs{sources_real},
      keys.m_account_address.m_spend_public_key,
      carrot::subaddress_index_extended{{}, carrot::AddressDeriveType::PreCarrot},
      {},
      {},
      proposal
    );

    const carrot::cryptonote_hierarchy_address_device_ram_borrowed cryptonote_device{
      keys.m_account_address.m_spend_public_key, keys.m_view_secret_key
    };

    crypto::hash tx_hash{};
    carrot::encrypted_payment_id_t enc_pid{};
    std::vector<crypto::key_image> images;
    std::vector<fcmp_pp::FcmpPpSalProof> proofs;
    std::vector<FcmpRerandomizedOutputCompressed> rerandomized;
    std::vector<carrot::RCTOutputEnoteProposal> enotes;
    {
      const carrot::generate_image_key_ram_borrowed_device generate_device{
        keys.m_spend_secret_key
      };
      const carrot::hybrid_hierarchy_address_device_composed hybrid_device{
        std::addressof(cryptonote_device), nullptr
      };
      const carrot::key_image_device_composed image_device{
        generate_device, hybrid_device, nullptr, std::addressof(cryptonote_device)
      };

      carrot::make_signable_tx_hash_from_proposal_v1(
        proposal, nullptr, std::addressof(cryptonote_device), image_device, tx_hash
      );

      std::vector<crypto::public_key> one_times;
      std::vector<rct::key> commitments;
      std::vector<rct::key> input_blinding;

      for (const auto& input : proposal.input_proposals)
      {
        one_times.push_back(onetime_address_ref(input));
        commitments.push_back(amount_commitment_ref(input));

        rct::xmr_amount amount;
        carrot::try_scan_opening_hint_amount(
          input,
          {std::addressof(keys.m_account_address.m_spend_public_key), 1},
          std::addressof(cryptonote_device),
          nullptr,
          amount,
          input_blinding.emplace_back()
        );
      }

      std::vector<std::size_t> order;
      carrot::get_sorted_input_key_images_from_proposal_v1(
        proposal, image_device, images, std::addressof(order)
      );

      carrot:get_output_enote_proposals_from_proposal_v1(
        proposal, nullptr, std::addressof(cryptonote_device), images.at(0), enotes, enc_pid
      );

      std::vector<rct::key> output_blinding;
      for (const auto& enote : enotes)
        output_blinding.push_back(rct::sk2rct(enote.amount_blinding_factor));

      carrot::make_carrot_rerandomized_outputs_nonrefundable(
        one_times, commitments, input_blinding, output_blinding, rerandomized
      );

      std::size_t i = -1;
      for (const auto& input : proposal.input_proposals)
      {
        ++i;
        crypto::key_image ignored;
        carrot::make_sal_proof_any_to_legacy_v1(
          tx_hash, rerandomized.at(i), input, keys.m_spend_secret_key, cryptonote_device, proofs.emplace_back(), ignored
        );
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

    return make_carrot_tx(lest_env, keys, {spend.pub, view.pub}, cache, tree, sources);
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
    const auto sub2_secret = hw.get_subaddress_secret_key(keys.m_view_secret_key, cryptonote::subaddress_index{0, 1});

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
    crypto::secret_key_to_public_key(incoming_key, account3.view_public);

    keys3.m_account_address.m_spend_public_key = account3.spend_public;
    keys3.m_account_address.m_view_public_key = account3.view_public;
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

    SECTION("lws::scanner::run")
    {
      const lws::db::block_id legacy_block_id = lws::db::block_id(std::uint64_t(last_block.id) + 2);

      const auto tree = fcmp_pp::curve_trees::curve_trees_v1();
      tree_cache cache(tree); 
      cache.init(std::uint64_t(last_block.id), last_block.hash, 0, {}, {});

      {
        const std::vector<lws::db::subaddress_dict> indexes{
          lws::db::subaddress_dict{
            lws::db::major_index::primary,
            lws::db::index_ranges{
              {lws::db::index_range{lws::db::minor_index(1), lws::db::minor_index(2)}}
            }
          }
        };
        const auto result =
          db.upsert_subaddresses(lws::db::account_id(1), account, keys.m_view_secret_key, indexes, 2);
        EXPECT(result);
        EXPECT(result->size() == 1);
        EXPECT(result->at(0).first == lws::db::major_index::primary);
        EXPECT(result->at(0).second.get_container().size() == 1);
        EXPECT(result->at(0).second.get_container().at(0).size() == 2);
        EXPECT(result->at(0).second.get_container().at(0).at(0) == lws::db::minor_index(1));
        EXPECT(result->at(0).second.get_container().at(0).at(1) == lws::db::minor_index(2));
      } 

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
          lws::db::output_id{0, 100}, 35184372088830, 0, 0, tx.pub_keys.at(0)
        },
        0,
        0,
        cryptonote::get_transaction_prefix_hash(tx.tx),
        tx.spend_publics.at(0),
        rct::commit(35184372088830, rct::identity()),
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
      bmessage.current_height = bmessage.start_height + 2;
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

      EXPECT(cache.register_output(make_output_pair(legacy_output)));
      cache.sync_block(
        bmessage.start_height,
        get_block_hash(bmessage.blocks.at(0).block),
        last_block.hash,
        {{bmessage.start_height + 1, {{100, 0, make_output_pair(legacy_output)}}}}
      );
      cache.sync_block(
        bmessage.start_height + 1,
        get_block_hash(bmessage.blocks.at(1).block),
        get_block_hash(bmessage.blocks.at(0).block),
        {}
      );
 
      // third block block
      transaction tx5 = make_carrot_tx(lest_env, keys, account3, cache, *tree, {legacy_output});

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

      EXPECT(db.add_account(account, keys.m_view_secret_key));
      EXPECT(db.add_account(account2, keys2.m_view_secret_key));
      EXPECT(db.add_account(account3, keys3.m_view_secret_key, lws::db::view_balance_key));

      messages.clear();
      messages.push_back(daemon_response(bmessage));
      bmessage.start_height = bmessage.current_height;
      bmessage.blocks.front() = bmessage.blocks.back();
      bmessage.blocks.resize(1);
      bmessage.output_indices.front() = bmessage.output_indices.back();
      bmessage.output_indices.resize(1);
      messages.push_back(daemon_response(bmessage));
      {
        static constexpr const lws::scanner_options opts{true, false};
        lws::scanner scanner{db.clone(), epee::net_utils::ssl_verification_t::none};
        boost::thread server_thread(&scanner_thread, std::ref(scanner), rpc.zmq_context(), std::cref(messages));
        const join on_scope_exit{server_thread};
        scanner.run(std::move(rpc), 1, {}, {}, opts);
      }
      lws_test::test_chain(lest_env, MONERO_UNWRAP(db.start_read()), last_block.id, epee::to_span(hashes));

      EXPECT(get_account().scan_height == lws::db::block_id(std::uint64_t(legacy_block_id) + 1));
      {
        static constexpr const std::uint64_t miner_reward = 35184372088830;
        const auto carrot1_block_id = lws::db::block_id(std::uint64_t(legacy_block_id) + 1);
        const std::uint8_t carrot1_index = tx5.carrot.at(0).amount == miner_reward / 2;
        const std::uint8_t carrot2_index = tx5.carrot.at(0).amount != miner_reward / 2;
        const std::uint64_t carrot1_amount = tx5.carrot.at(carrot1_index).amount;
        const std::uint64_t carrot2_amount = tx5.carrot.at(carrot2_index).amount;
        const std::map<std::pair<lws::db::output_id, std::uint32_t>, lws::db::output> expected{
          {
            {lws::db::output_id{0, 100}, miner_reward}, legacy_output 
          },
          {
            {lws::db::output_id{0, 101}, 8000}, lws::db::output{
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
            {lws::db::output_id{0, 102}, 8000}, lws::db::output{
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
            {lws::db::output_id{0, 200}, 8000}, lws::db::output{
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
            {lws::db::output_id{0, 201}, 8000}, lws::db::output{
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
            {lws::db::output_id{0, 200}, 2000}, lws::db::output{
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
            {lws::db::output_id{0, 201}, 2000}, lws::db::output{
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
            {lws::db::output_id{0, std::uint64_t(301 + carrot1_index)}, carrot1_amount}, lws::db::output{
              lws::db::transaction_link{carrot1_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(301 + carrot1_index)},
                carrot1_amount,
                lws::db::carrot_external,
                carrot1_index,
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
          EXPECT(real_output.first_image == expected_output->second.first_image);
          EXPECT(real_output.anchor == expected_output->second.anchor);
        }

        const std::map<std::pair<lws::db::output_id, std::uint32_t>, lws::db::output> expected2{
          {
            {lws::db::output_id{0, std::uint64_t(301 + carrot2_index)}, carrot2_amount}, lws::db::output{
              lws::db::transaction_link{carrot1_block_id, cryptonote::get_transaction_hash(tx5.tx)},
              lws::db::output::spend_meta_{
                lws::db::output_id{0, std::uint64_t(301 + carrot2_index)},
                carrot2_amount,
                lws::db::carrot_external,
                carrot2_index,
                carrot::raw_byte_convert<crypto::public_key>(
                  tx5.carrot.at(carrot1_index).enote.enote_ephemeral_pubkey
                )
              },
              0,
              0,
              cryptonote::get_transaction_prefix_hash(tx5.tx),
              tx5.carrot.at(carrot2_index).enote.onetime_address,
              tx5.carrot.at(carrot2_index).enote.amount_commitment,
              {},
              lws::db::pack(lws::db::extra::ringct_output, 8),
              {},
              519840000, // fee
              lws::db::address_index{},
              tx5.carrot.at(carrot2_index).enote.tx_first_key_image,
              tx5.carrot.at(carrot2_index).enote.anchor_enc
            }
          }
        };

        outputs = MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(3)));
        EXPECT(outputs.count() == 1);
        output_it = outputs.make_iterator();
        for (auto output_it = outputs.make_iterator(); !output_it.is_end(); ++output_it)
        {
          auto real_output = *output_it;
          const auto expected_output =
            expected2.find(std::make_pair(real_output.spend_meta.id, real_output.spend_meta.amount));
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
          EXPECT(real_output.first_image == expected_output->second.first_image);
          EXPECT(real_output.anchor == expected_output->second.anchor);
        }

        auto spends = MONERO_UNWRAP(reader.get_spends(lws::db::account_id(1)));
        EXPECT(spends.count() == 3);
        auto spend_it = spends.make_iterator();
        EXPECT(!spend_it.is_end());

        auto real_spend = *spend_it;
        EXPECT(real_spend.link.height == legacy_block_id);
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
        EXPECT(real_spend.link.height == legacy_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx3.tx));
        expected_out = lws::db::output_id{0, 101};
        EXPECT(real_spend.source == expected_out);
        EXPECT(real_spend.mixin_count == 15);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        ++spend_it;
        EXPECT(!spend_it.is_end());

        real_spend = *spend_it;
        EXPECT(real_spend.link.height == carrot1_block_id);
        EXPECT(real_spend.link.tx_hash == cryptonote::get_transaction_hash(tx5.tx));
        EXPECT(real_spend.source == lws::db::output_id::unknown_spend());
        EXPECT(real_spend.mixin_count == lws::db::carrot_external);
        EXPECT(real_spend.length == 0);
        EXPECT(real_spend.payment_id == crypto::hash{});
        EXPECT(real_spend.sender == lws::db::address_index{});

        EXPECT(MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(2))).count() == 0);
        EXPECT(MONERO_UNWRAP(reader.get_spends(lws::db::account_id(2))).count() == 0);

        EXPECT(MONERO_UNWRAP(reader.get_outputs(lws::db::account_id(3))).count() == 1);
        EXPECT(MONERO_UNWRAP(reader.get_spends(lws::db::account_id(3))).count() == 0);
      }
    } //SECTION (lws::scanner::run)
  } // SETUP
} // LWS_CASE

