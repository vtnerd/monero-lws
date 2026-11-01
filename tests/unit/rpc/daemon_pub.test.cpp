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

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>
#include "byte_stream.h"                              // monero/contrib/epee/include
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "fcmp_pp/fcmp_pp_types.h"                    // monero/src
#include "ringct/rctSigs.h"                           // monero/src
#include "rpc/daemon_pub.h"
#include "serialization/json_object.h"                // monero/src

namespace
{
  cryptonote::transaction make_fcmp_pp_tx()
  {
    const std::size_t n_inputs = 2;
    const std::size_t n_outputs = 2;
    const std::uint8_t n_tree_layers = 3;

    cryptonote::transaction tx{};
    tx.version = 2;
    tx.rct_signatures.type = rct::RCTTypeFcmpPlusPlus;
    tx.rct_signatures.txnFee = 1000;

    for (std::size_t i = 0; i < n_inputs; ++i)
    {
      cryptonote::txin_to_key in{};
      in.k_image = rct::rct2ki(rct::pkGen());
      tx.vin.push_back(in);
    }

    std::vector<std::uint64_t> amounts;
    for (std::size_t i = 0; i < n_outputs; ++i)
    {
      cryptonote::tx_out out{};
      cryptonote::set_tx_out(0, rct::rct2pk(rct::pkGen()), true, crypto::view_tag{}, out);
      tx.vout.push_back(out);
      tx.rct_signatures.ecdhInfo.push_back({rct::zero(), rct::skGen()});
      tx.rct_signatures.outPk.push_back({rct::zero(), rct::pkGen()});
      amounts.push_back(i + 1);
    }

    rct::keyV C, masks;
    tx.rct_signatures.p.bulletproofs_plus.push_back(rct::make_dummy_bulletproof_plus(amounts, C, masks));
    for (std::size_t i = 0; i < n_inputs; ++i)
      tx.rct_signatures.p.pseudoOuts.push_back(rct::pkGen());

    tx.rct_signatures.p.reference_block = 3118750;
    tx.rct_signatures.p.n_tree_layers = n_tree_layers;
    tx.rct_signatures.p.fcmp_pp.resize(fcmp_pp::fcmp_pp_proof_len(n_inputs, n_tree_layers));
    for (std::size_t i = 0; i < tx.rct_signatures.p.fcmp_pp.size(); ++i)
      tx.rct_signatures.p.fcmp_pp[i] = std::uint8_t(i);
    return tx;
  }

  //! Body of a `json-full-txpool_add` pub, as written by monerod
  std::string txpool_json(const cryptonote::transaction& tx)
  {
    epee::byte_stream buffer;
    {
      rapidjson::Writer<epee::byte_stream> dest{buffer};
      cryptonote::json::toJsonValue(dest, std::vector<cryptonote::transaction>{tx});
    }
    return std::string{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
  }

  //! Remove `fields` from the `prunable` object of the first tx
  std::string remove_fields(const std::string& json, const std::vector<const char*>& fields)
  {
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    auto& prunable = doc[0]["ringct"]["prunable"];
    for (const char* field : fields)
      prunable.RemoveMember(field);

    epee::byte_stream buffer;
    {
      rapidjson::Writer<epee::byte_stream> dest{buffer};
      doc.Accept(dest);
    }
    return std::string{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
  }
}

LWS_CASE("lws::rpc::full_txpool_pub")
{
  SETUP("FCMP++ transaction")
  {
    const cryptonote::transaction tx = make_fcmp_pp_tx();
    const std::string json = txpool_json(tx);

    SECTION("FCMP++ data keeps the tx hash")
    {
      auto pub = lws::rpc::full_txpool_pub::from_json(std::string{json});
      EXPECT(pub);
      EXPECT(pub->txes.size() == 1);

      const auto& copy = pub->txes.at(0).rct_signatures.p;
      EXPECT(copy.reference_block == tx.rct_signatures.p.reference_block);
      EXPECT(copy.n_tree_layers == tx.rct_signatures.p.n_tree_layers);
      EXPECT(copy.fcmp_pp == tx.rct_signatures.p.fcmp_pp);
      EXPECT(cryptonote::get_transaction_hash(pub->txes.at(0)) == cryptonote::get_transaction_hash(tx));
    }

    SECTION("No FCMP++ data (older monerod)")
    {
      auto pub = lws::rpc::full_txpool_pub::from_json(remove_fields(json, {"reference_block", "n_tree_layers", "fcmp_pp"}));
      EXPECT(pub);
      EXPECT(pub->txes.size() == 1);
      EXPECT(pub->txes.at(0).rct_signatures.p.fcmp_pp.empty());
    }

    SECTION("Partial FCMP++ data")
    {
      EXPECT(!lws::rpc::full_txpool_pub::from_json(remove_fields(json, {"fcmp_pp"})));
    }
  }
}
