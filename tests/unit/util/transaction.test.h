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

#pragma once

#include <cstdint>
#include <vector>

#include "cryptonote_basic/cryptonote_basic.h"   // monero/src
#include "db/fwd.h"
#include "lest.hpp"

namespace cryptonote
{
  class account_keys;
  struct public_key;
  class secret_key;
  class tx_destination_entry;
}
namespace rct { struct key; }

namespace lws_test
{
  struct transaction
  {
    cryptonote::transaction tx;
    std::vector<crypto::secret_key> additional_keys;
    std::vector<crypto::public_key> pub_keys;
    std::vector<crypto::public_key> spend_publics;
    std::vector<crypto::key_image> images;
    std::vector<rct::key> ringct;
  };

  transaction make_miner_tx(lest::env& lest_env, lws::db::block_id height, const lws::db::account_address& miner_address, bool use_view_tags);
  transaction make_tx(lest::env& lest_env, const cryptonote::account_keys& keys, std::vector<cryptonote::tx_destination_entry> destinations, const std::uint32_t ring_base, const bool use_view_tag);
}
