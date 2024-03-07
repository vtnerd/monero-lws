// Copyright (c) 2024, The Monero Project
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

#include "blocks.h"

#include "cryptonote_config.h" // monero/src
#include "crypto/hash-ops.h"   // monero/src
#include "db/storage.h"
#include "error.h"
#include "misc_language.h"
#include "string_tools.h"

namespace lws
{
  crypto::hash get_block_longhash(
    const std::string& bd, const db::block_id height, const unsigned major_version, const db::storage& disk, const db::block_id cached_start, epee::span<const crypto::hash> cached)
  {
    crypto::hash result{};

    // block 202612 bug workaround
    if (height == db::block_id(202612))
    {
      static const std::string longhash_202612 = "84f64766475d51837ac9efbef1926486e58563c95a19fef4aec3254f03000000";
      epee::string_tools::hex_to_pod(longhash_202612, result);
      return result;
    }
    if (major_version >= RX_BLOCK_VERSION)
    {
      crypto::hash hash{};
      if (height != db::block_id(0))
      {
        const uint64_t seed_height = crypto::rx_seedheight(std::uint64_t(height));
        if (cached_start <= db::block_id(seed_height))
        {
          if (cached.size() <= seed_height - std::uint64_t(cached_start))
            MONERO_THROW(error::bad_blockchain, "invalid seed_height for cache or DB");
          hash = cached[seed_height - std::uint64_t(cached_start)];
        }
        else
          hash = MONERO_UNWRAP(MONERO_UNWRAP(disk.start_read()).get_block_hash(db::block_id(seed_height)));
      }
      else
      {
        memset(&result, 0, sizeof(crypto::hash));  // only happens when generating genesis block
      }
      crypto::rx_slow_hash(hash.data, bd.data(), bd.size(), result.data);
    } else {
      const int pow_variant = major_version >= 7 ? major_version - 6 : 0;
      crypto::cn_slow_hash(bd.data(), bd.size(), result, pow_variant, std::uint64_t(height));
    }
    return result;
  }

  bool verify_timestamp(std::uint64_t check, std::vector<std::uint64_t> timestamps)
  {
    if (timestamps.empty())
      return true;
    if(check < epee::misc_utils::median(timestamps))
      return false;
    return true;
  }
}

