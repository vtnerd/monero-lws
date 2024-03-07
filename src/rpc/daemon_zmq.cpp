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
#include "crypto/crypto.h"            // monero/src
#include "rpc/message_data_structs.h" // monero/src
#include "wire/crypto.h"
#include "wire/json.h"
#include "wire/wrapper/variant.h"
#include "wire/vector.h"

namespace
{
  constexpr const std::size_t default_blocks_fetched = 1000;
  constexpr const std::size_t default_transaction_count = 100;
  constexpr const std::size_t default_inputs = 2;
  constexpr const std::size_t default_outputs = 4;
  constexpr const std::size_t default_txextra_size = 2048;
  constexpr const std::size_t default_txpool_size = 32;
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
    wire::object(source, WIRE_FIELD(ss), WIRE_FIELD(cc));
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
      wire::object(source,
        wire::field("range_proofs", std::ref(self.prunable.rangeSigs)),
        wire::field("bulletproofs", std::ref(self.prunable.bulletproofs)),
        wire::field("bulletproofs_plus", std::ref(self.prunable.bulletproofs_plus)),
        wire::field("mlsags", std::ref(self.prunable.MGs)),
        wire::field("clsags", std::ref(self.prunable.CLSAGs)),
        wire::field("pseudo_outs", std::ref(self.pseudo_outs))
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
    boost::optional<std::vector<ecdhTuple>> ecdhInfo;
    boost::optional<ctkeyV> outPk;
    boost::optional<xmr_amount> txnFee;
    boost::optional<prunable_helper> prunable;
    self.outPk.reserve(default_inputs);
    wire::object(source,
      WIRE_FIELD(type),
      wire::optional_field("encrypted", std::ref(ecdhInfo)),
      wire::optional_field("commitments", std::ref(outPk)),
      wire::optional_field("fee", std::ref(txnFee)),
      wire::optional_field("prunable", std::ref(prunable))
    );

    self.txnFee = 0;
    if (self.type != RCTTypeNull)
    {
      if (!ecdhInfo || !outPk || !txnFee)
        WIRE_DLOG_THROW(wire::error::schema::missing_key, "Expected fields `encrypted`, `commitments`, and `fee`");
      self.ecdhInfo = std::move(*ecdhInfo);
      self.outPk = std::move(*outPk);
      self.txnFee = std::move(*txnFee);
    }
    else if (ecdhInfo || outPk || txnFee)
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
  static void read_bytes(wire::json_reader& source, txout_to_script& self)
  {
    wire::object(source, WIRE_FIELD(keys), WIRE_FIELD(script));
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
      WIRE_OPTION("to_script", txout_to_script, variant),
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
    wire::object(source, WIRE_FIELD(prev), WIRE_FIELD(prevout), WIRE_FIELD(script), WIRE_FIELD(sigset));
  }
  static void read_bytes(wire::json_reader& source, txin_to_key& self)
  {
    wire::object(source, WIRE_FIELD(amount), WIRE_FIELD(key_offsets), wire::field("key_image", std::ref(self.k_image)));
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
      wire::field("inputs", std::ref(self.vin)),
      wire::field("outputs", std::ref(self.vout)),
      WIRE_FIELD(extra),
      wire::field("ringct", std::ref(self.rct_signatures))
    );
  }

  static void read_bytes(wire::json_reader& source, block& self)
  {
    self.tx_hashes.reserve(default_transaction_count);
    wire::object(source,
      WIRE_FIELD(major_version),
      WIRE_FIELD(minor_version),
      WIRE_FIELD(timestamp),
      WIRE_FIELD(miner_tx),
      WIRE_FIELD(tx_hashes),
      WIRE_FIELD(prev_id),
      WIRE_FIELD(nonce)
    );
  }

  namespace rpc
  {
    static void read_bytes(wire::json_reader& source, block_with_transactions& self)
    {
      self.transactions.reserve(default_transaction_count);
      wire::object(source, WIRE_FIELD(block), WIRE_FIELD(transactions));
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
  wire::object(source, WIRE_FIELD(blocks), WIRE_FIELD(output_indices), WIRE_FIELD(start_height), WIRE_FIELD(current_height));
}

void lws::rpc::read_bytes(wire::json_reader& source, get_transaction_pool_response& self)
{
  self.transactions.reserve(default_txpool_size);
  wire::object(source, WIRE_FIELD(transactions));
}
