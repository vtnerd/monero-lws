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
#include "wire/vector.h"

namespace
{
  constexpr const std::size_t default_blocks_fetched = 1000;
  constexpr const std::size_t default_transaction_count = 100;
  constexpr const std::size_t default_inputs = 2;
  constexpr const std::size_t default_outputs = 4;
  constexpr const std::size_t default_txextra_size = 2048;
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

  static void read_bytes(wire::json_reader& source, rctSig& self)
  {
    boost::optional<std::vector<ecdhTuple>> ecdhInfo;
    boost::optional<ctkeyV> outPk;
    boost::optional<xmr_amount> txnFee;

    self.outPk.reserve(default_inputs);
    wire::object(source,
      WIRE_FIELD(type),
      wire::optional_field("encrypted", std::ref(ecdhInfo)),
      wire::optional_field("commitments", std::ref(outPk)),
      wire::optional_field("fee", std::ref(txnFee))
    );

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
    wire::object(source,
      WIRE_FIELD(amount),
      wire::variant_field("transaction output variant", std::ref(self.target),
        wire::option<txout_to_key>{"to_key"},
        wire::option<txout_to_tagged_key>{"to_tagged_key"},
        wire::option<txout_to_script>{"to_script"},
        wire::option<txout_to_scripthash>{"to_scripthash"}
      )
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
    wire::object(source,
      wire::variant_field("transaction input variant", std::ref(self),
        wire::option<txin_to_key>{"to_key"},
        wire::option<txin_gen>{"gen"},
        wire::option<txin_to_script>{"to_script"},
        wire::option<txin_to_scripthash>{"to_scripthash"}
      )
    );
  }

  static void read_bytes(wire::json_reader& source, transaction& self)
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
  } // rpc
} // cryptonote

void lws::rpc::read_bytes(wire::json_reader& source, get_blocks_fast_response& self)
{
  self.blocks.reserve(default_blocks_fetched);
  self.output_indices.reserve(default_blocks_fetched);
  wire::object(source, WIRE_FIELD(blocks), WIRE_FIELD(output_indices), WIRE_FIELD(start_height), WIRE_FIELD(current_height));
}

