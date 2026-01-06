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

#include "daemon_zmq.h"

#include <boost/optional/optional.hpp>
#include "cryptonote_config.h"        // monero/src
#include "crypto/crypto.h"            // monero/src
#include "rpc/message_data_structs.h" // monero/src
#include "wire/adapted/carrot.h"
#include "wire/adapted/crypto.h"
#include "wire/json.h"
#include "wire/wrapper/array.h"
#include "wire/wrapper/variant.h"
#include "wire/wrappers_impl.h"
#include "wire/vector.h"

namespace
{
  constexpr const std::size_t default_blocks_fetched = 1000;
  constexpr const std::size_t default_transaction_count = 100;
  constexpr const std::size_t default_inputs = 2;
  constexpr const std::size_t default_outputs = 4;
  constexpr const std::size_t default_txextra_size = 2048;
  constexpr const std::size_t default_txpool_size = 32;

  using max_blocks_per_fetch =
    wire::max_element_count<COMMAND_RPC_GET_BLOCKS_FAST_MAX_BLOCK_COUNT>;

  //! Not the default in cryptonote, but roughly a 31.8 MiB block
  using max_txes_per_block = wire::max_element_count<21845>; 

  using max_inputs_per_tx = wire::max_element_count<3000>;
  using max_outputs_per_tx = wire::max_element_count<2000>;
  using max_ring_size = wire::max_element_count<4600>;
  using max_txpool_size = wire::max_element_count<700>;
}

namespace rct
{
  static void read_bytes(wire::json_reader& source, ctkey& self)
  {
    self.dest = {};
    read_bytes(source, self.mask);
  }

  static void read_bytes(wire::json_reader& source, ecdhTuple& self)
  {
    wire::object(source, WIRE_FIELD(mask), WIRE_FIELD(amount));
  }

  static void read_bytes(wire::json_reader& source, clsag& self)
  {
    wire::object(source, WIRE_FIELD(s), WIRE_FIELD(c1), WIRE_FIELD(D));
  }

  static void read_bytes(wire::json_reader& source, mgSig& self)
  {
    using max_256 = wire::max_element_count<256>;
    wire::object(source,
      wire::field("ss", wire::array<max_256>(std::ref(self.ss))),
      WIRE_FIELD(cc)
    );
  }

  static void read_bytes(wire::json_reader& source, BulletproofPlus& self)
  {
    wire::object(source,
      WIRE_FIELD(V),
      WIRE_FIELD(A),
      WIRE_FIELD(A1),
      WIRE_FIELD(B),
      WIRE_FIELD(r1),
      WIRE_FIELD(s1),
      WIRE_FIELD(d1),
      WIRE_FIELD(L),
      WIRE_FIELD(R)
    );
  }

  static void read_bytes(wire::json_reader& source, Bulletproof& self)
  {
    wire::object(source,
      WIRE_FIELD(V),
      WIRE_FIELD(A),
      WIRE_FIELD(S),
      WIRE_FIELD(T1),
      WIRE_FIELD(T2),
      WIRE_FIELD(taux),
      WIRE_FIELD(mu),
      WIRE_FIELD(L),
      WIRE_FIELD(R),
      WIRE_FIELD(a),
      WIRE_FIELD(b),
      WIRE_FIELD(t)
    );
  }

  static void read_bytes(wire::json_reader& source, boroSig& self)
  {
    std::vector<rct::key> s0;
    std::vector<rct::key> s1;
    s0.reserve(64);
    s1.reserve(64);
    wire::object(source, wire::field("s0", std::ref(s0)), wire::field("s1", std::ref(s1)));

    if (s0.size() != 64 || s1.size() != 64)
      WIRE_DLOG_THROW(wire::error::schema::array, "Expected s0 and s1 to have 64 elements");

    for (std::size_t i = 0; i < 64; ++i)
      self.s0[i] = s0[i];
    for (std::size_t i = 0; i < 64; ++i)
      self.s1[i] = s1[i];
  }

  static void read_bytes(wire::json_reader& source, rangeSig& self)
  {
    std::vector<rct::key> keys{};
    keys.reserve(64);

    wire::object(source, WIRE_FIELD(asig), wire::field("Ci", std::ref(keys)));
    if (keys.size() != 64)
      WIRE_DLOG_THROW(wire::error::schema::array, "Expected 64 eleents in Ci");
    for (std::size_t i = 0; i < 64; ++i)
    {
      self.Ci[i] = keys[i];
    }
  }

  namespace
  {
    struct prunable_helper
    {
      rctSigPrunable prunable;
      rct::keyV pseudo_outs;
    };

    void read_bytes(wire::json_reader& source, prunable_helper& self)
    {
      using rf_min_size = wire::min_element_sizeof<key64, key64, key64, key>;
      using bf_max = wire::max_element_count<BULLETPROOF_MAX_OUTPUTS>;
      using bf_plus_max = wire::max_element_count<BULLETPROOF_PLUS_MAX_OUTPUTS>;
      using mlsags_max = max_inputs_per_tx;
      using clsags_max = max_inputs_per_tx;
      using pseudo_outs_min_size = wire::min_element_sizeof<key>;

      wire::object(source,
        wire::field("range_proofs", wire::array<rf_min_size>(std::ref(self.prunable.rangeSigs))),
        wire::field("bulletproofs", wire::array<bf_max>(std::ref(self.prunable.bulletproofs))),
        wire::field("bulletproofs_plus", wire::array<bf_plus_max>(std::ref(self.prunable.bulletproofs_plus))),
        wire::field("mlsags", wire::array<mlsags_max>(std::ref(self.prunable.MGs))),
        wire::field("clsags", wire::array<clsags_max>(std::ref(self.prunable.CLSAGs))),
        wire::field("pseudo_outs", wire::array<pseudo_outs_min_size>(std::ref(self.pseudo_outs)))
      );

      const bool pruned =
        self.prunable.rangeSigs.empty() &&
        self.prunable.bulletproofs.empty() &&
        self.prunable.bulletproofs_plus.empty() &&
        self.prunable.MGs.empty() &&
        self.prunable.CLSAGs.empty() &&
        self.pseudo_outs.empty();

      if (pruned)
        WIRE_DLOG_THROW(wire::error::schema::array, "Expected at least one prunable field");
    }
  } // anonymous

  static void read_bytes(wire::json_reader& source, rctSig& self)
  {
    using min_ecdh = wire::min_element_sizeof<rct::key, rct::key>;
    using min_ctkey = wire::min_element_sizeof<rct::key>;
 
    boost::optional<xmr_amount> txnFee;
    boost::optional<prunable_helper> prunable;
    self.outPk.reserve(default_inputs);
    wire::object(source,
      WIRE_FIELD(type),
      wire::optional_field("encrypted", wire::array<min_ecdh>(std::ref(self.ecdhInfo))),
      wire::optional_field("commitments", wire::array<min_ctkey>(std::ref(self.outPk))),
      wire::optional_field("fee", std::ref(txnFee)),
      wire::optional_field("prunable", std::ref(prunable))
    );

    self.txnFee = 0;
    if (self.type != RCTTypeNull)
    {
      if (self.ecdhInfo.empty() || self.outPk.empty() || !txnFee)
        WIRE_DLOG_THROW(wire::error::schema::missing_key, "Expected fields `encrypted`, `commitments`, and `fee`");
      self.txnFee = std::move(*txnFee);
    }
    else if (!self.ecdhInfo.empty() || !self.outPk.empty() || txnFee)
      WIRE_DLOG_THROW(wire::error::schema::invalid_key, "Did not expected `encrypted`, `commitments`, or `fee`");

    if (prunable)
    {
      self.p = std::move(prunable->prunable);
      self.get_pseudo_outs() = std::move(prunable->pseudo_outs);
    }
  }
} // rct

namespace cryptonote
{
  static void read_bytes(wire::json_reader& source, txout_to_carrot_v1& self)
  {
    wire::object(source, WIRE_FIELD(key), WIRE_FIELD(view_tag), WIRE_FIELD(encrypted_janus_anchor));
  }
  static void read_bytes(wire::json_reader& source, txout_to_scripthash& self)
  {
    wire::object(source, WIRE_FIELD(hash));
  }
  static void read_bytes(wire::json_reader& source, txout_to_tagged_key& self)
  {
    wire::object(source, WIRE_FIELD(key), WIRE_FIELD(view_tag));
  }
  static void read_bytes(wire::json_reader& source, txout_to_key& self)
  {
    wire::object(source, WIRE_FIELD(key));
  }
  static void read_bytes(wire::json_reader& source, tx_out& self)
  {
    auto variant = wire::variant(std::ref(self.target));
    wire::object(source,
      WIRE_FIELD(amount),
      WIRE_OPTION("to_key", txout_to_key, variant),
      WIRE_OPTION("to_tagged_key", txout_to_tagged_key, variant),
      WIRE_OPTION("to_carrot_v1", txout_to_carrot_v1, variant),
      WIRE_OPTION("to_scripthash", txout_to_scripthash, variant)
    );
  }

  static void read_bytes(wire::json_reader& source, txin_gen& self)
  {
    wire::object(source, WIRE_FIELD(height));
  }
  static void read_bytes(wire::json_reader& source, txin_to_script& self)
  {
    wire::object(source, WIRE_FIELD(prev), WIRE_FIELD(prevout), WIRE_FIELD(sigset));
  }
  static void read_bytes(wire::json_reader& source, txin_to_scripthash& self)
  {
    wire::object(source);
  }
  static void read_bytes(wire::json_reader& source, txin_to_key& self)
  {
    wire::object(source,
      WIRE_FIELD(amount),
      WIRE_FIELD_ARRAY(key_offsets, max_ring_size),
      wire::field("key_image", std::ref(self.k_image))
    );
  }
  static void read_bytes(wire::json_reader& source, txin_v& self)
  {
    auto variant = wire::variant(std::ref(self));
    wire::object(source,
      WIRE_OPTION("to_key", txin_to_key, variant),
      WIRE_OPTION("gen", txin_gen, variant),
      WIRE_OPTION("to_script", txin_to_script, variant),
      WIRE_OPTION("to_scripthash", txin_to_scripthash, variant)
    );
  }

  void read_bytes(wire::json_reader& source, transaction& self)
  {
    self.vin.reserve(default_inputs);
    self.vout.reserve(default_outputs);
    self.extra.reserve(default_txextra_size);
    wire::object(source,
      WIRE_FIELD(version),
      WIRE_FIELD(unlock_time),
      wire::field("inputs", wire::array<max_inputs_per_tx>(std::ref(self.vin))),
      wire::field("outputs", wire::array<max_outputs_per_tx>(std::ref(self.vout))),
      WIRE_FIELD(extra),
      WIRE_FIELD_ARRAY(signatures, max_inputs_per_tx),
      wire::field("ringct", std::ref(self.rct_signatures))
    );
  }

  static void read_bytes(wire::json_reader& source, block& self)
  {
    std::optional<std::uint8_t> n_tree_layers;
    std::optional<crypto::ec_point> tree_root;
    using min_hash_size = wire::min_element_sizeof<crypto::hash>;
    self.tx_hashes.reserve(default_transaction_count);
    wire::object(source,
      WIRE_FIELD(major_version),
      WIRE_FIELD(minor_version),
      WIRE_FIELD(timestamp),
      WIRE_FIELD(miner_tx),
      WIRE_FIELD_ARRAY(tx_hashes, min_hash_size),
      WIRE_FIELD(prev_id),
      WIRE_FIELD(nonce),
      wire::optional_field("fcmp_pp_n_tree_layers", std::ref(n_tree_layers)),
      wire::optional_field("fcmp_pp_tree_root", std::ref(tree_root))
    );
    if (self.major_version >= HF_VERSION_FCMP_PLUS_PLUS)
    {
      if (!n_tree_layers || !tree_root)
        WIRE_DLOG_THROW(wire::error::schema::binary, "Expected fcmp++ elements");
      self.fcmp_pp_n_tree_layers = *n_tree_layers;
      self.fcmp_pp_tree_root = *tree_root;
    }
  }

  static void read_bytes(wire::json_reader& source, std::vector<transaction>& self)
  {
    wire_read::array_unchecked(source, self, 0, max_txes_per_block{});
  }

  namespace rpc
  { 
    static void read_bytes(wire::json_reader& source, block_with_transactions& self)
    {
      self.transactions.reserve(default_transaction_count);
      wire::object(source, WIRE_FIELD(block), WIRE_FIELD(transactions));
    }

    static void read_bytes(wire::json_reader& source, std::vector<block_with_transactions>& self)
    {
      wire_read::array_unchecked(source, self, 0, max_blocks_per_fetch{}); 
    }

    static void read_bytes(wire::json_reader& source, tx_in_pool& self)
    {
      wire::object(source, WIRE_FIELD(tx), WIRE_FIELD(tx_hash));
    }
  } // rpc
} // cryptonote

void lws::rpc::read_bytes(wire::json_reader& source, get_hashes_fast_response& self)
{
  self.hashes.reserve(default_blocks_fetched);
  wire::object(source, WIRE_FIELD(hashes), WIRE_FIELD(start_height), WIRE_FIELD(current_height));
}

void lws::rpc::read_bytes(wire::json_reader& source, get_blocks_fast_response& self)
{
  self.blocks.reserve(default_blocks_fetched);
  self.output_indices.reserve(default_blocks_fetched);
  wire::object(source,
    WIRE_FIELD(blocks),
    wire::field("output_indices", wire::array<max_blocks_per_fetch>(wire::array<max_txes_per_block>(wire::array<max_outputs_per_tx>(std::ref(self.output_indices))))),
    wire::field("unified_indices", wire::array<max_blocks_per_fetch>(wire::array<max_txes_per_block>(wire::array<max_outputs_per_tx>(std::ref(self.unified_indices))))),
    WIRE_FIELD(start_height),
    WIRE_FIELD(current_height)
  );
}

void lws::rpc::read_bytes(wire::json_reader& source, get_transaction_pool_response& self)
{
  self.transactions.reserve(default_txpool_size);
  wire::object(source, WIRE_FIELD_ARRAY(transactions, max_txpool_size));
}
