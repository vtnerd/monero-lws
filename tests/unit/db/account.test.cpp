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

#include "framework.test.h"

#include "crypto/crypto.h"            // monero/src
#include "cryptonote_basic/account.h" // monero/src
#include "db/account.h"
#include "wire/msgpack.h"

LWS_CASE("lws::account serialization")
{
  cryptonote::account_keys keys{};
  crypto::generate_keys(keys.m_account_address.m_spend_public_key, keys.m_spend_secret_key);
  crypto::generate_keys(keys.m_account_address.m_view_public_key, keys.m_view_secret_key);

  lws::db::account db_account{
    lws::db::account_id(44),
    lws::db::account_time(500000),
    lws::db::account_address{
      keys.m_account_address.m_view_public_key,
      keys.m_account_address.m_spend_public_key
    },
    {},
    lws::db::block_id(1000000),
    lws::db::block_id(500000),
    lws::db::account_time(250000)
  };
  std::memcpy(
    std::addressof(db_account.key),
    std::addressof(unwrap(unwrap(keys.m_view_secret_key))),
    sizeof(db_account.key)
  );

  const std::vector<std::pair<lws::db::output_id, lws::db::address_index>> spendable{
    {
      lws::db::output_id{100, 2000},
      lws::db::address_index{lws::db::major_index(1), lws::db::minor_index(34)}
    }
  };
  const std::vector<crypto::public_key> pubs{crypto::rand<crypto::public_key>()};

  lws::account account{db_account, spendable, {}, pubs};
  EXPECT(account);

  const lws::db::transaction_link link{
    lws::db::block_id(4000), crypto::rand<crypto::hash>()
  };
  const crypto::public_key tx_public = crypto::rand<crypto::public_key>();
  const crypto::hash tx_prefix = crypto::rand<crypto::hash>();
  const crypto::public_key pub = crypto::rand<crypto::public_key>();
  const rct::key ringct = crypto::rand<rct::key>();
  const auto extra =
    lws::db::extra(lws::db::extra::coinbase_output | lws::db::extra::ringct_output);
  const auto payment_id_ = crypto::rand<lws::db::output::payment_id_>();
  const crypto::hash payment_id = crypto::rand<crypto::hash>();
  const crypto::key_image image = crypto::rand<crypto::key_image>();

  account.add_out(
    lws::db::output{
      link,
      lws::db::output::spend_meta_{
        lws::db::output_id{500, 30},
        std::uint64_t(40000),
        std::uint32_t(16),
        std::uint32_t(2),
        tx_public
      },
      std::uint64_t(7000),
      std::uint64_t(4670),
      tx_prefix,
      pub,
      ringct,
      {0, 0, 0, 0, 0, 0, 0},
      lws::db::pack(extra, sizeof(crypto::hash)),
      payment_id_,
      std::uint64_t(33444),
      lws::db::address_index{lws::db::major_index(2), lws::db::minor_index(66)}
    }
  );
  account.add_spend(
    lws::db::spend{
      link,
      image,
      lws::db::output_id{10, 70000},
      std::uint64_t(66),
      std::uint64_t(1500),
      std::uint32_t(16),
      {0, 0, 0},
      32,
      payment_id,
      lws::db::address_index{lws::db::major_index(4), lws::db::minor_index(55)}     
    }
  );

  const std::string account_address = account.address();
  EXPECT(account.id() == db_account.id);
  EXPECT(account.view_public() == db_account.address.view_public);
  EXPECT(account.spend_public() == db_account.address.spend_public);
  EXPECT(account.view_key() == keys.m_view_secret_key);
  EXPECT(account.scan_height() == db_account.scan_height);
  {
    const auto result = account.get_spendable(lws::db::output_id{100, 2000});
    EXPECT(bool(result));
    EXPECT(result->maj_i == lws::db::major_index(1));
    EXPECT(result->min_i == lws::db::minor_index(34));
  }
  EXPECT(account.outputs().size() == 1);
  EXPECT(account.outputs()[0].link == link);
  EXPECT(account.outputs()[0].spend_meta.id.high == 500);
  EXPECT(account.outputs()[0].spend_meta.id.low == 30);
  EXPECT(account.outputs()[0].spend_meta.amount == 40000);
  EXPECT(account.outputs()[0].spend_meta.mixin_count == 16);
  EXPECT(account.outputs()[0].spend_meta.index == 2);
  EXPECT(account.outputs()[0].spend_meta.tx_public == tx_public);
  EXPECT(account.outputs()[0].timestamp == 7000);
  EXPECT(account.outputs()[0].unlock_time == 4670);
  EXPECT(account.outputs()[0].tx_prefix_hash == tx_prefix);
  EXPECT(account.outputs()[0].pub == pub);
  EXPECT(account.outputs()[0].ringct_mask == ringct);
  {
    const auto unpacked = lws::db::unpack(account.outputs()[0].extra);
    EXPECT(unpacked.first == extra);
    EXPECT(unpacked.second == sizeof(crypto::hash));
  }
  EXPECT(account.outputs()[0].recipient.maj_i == lws::db::major_index(2));
  EXPECT(account.outputs()[0].recipient.min_i == lws::db::minor_index(66));

  EXPECT(account.spends().size() == 1);
  EXPECT(account.spends()[0].link == link);
  EXPECT(account.spends()[0].image == image);
  EXPECT(account.spends()[0].source.high == 10);
  EXPECT(account.spends()[0].source.low == 70000);
  EXPECT(account.spends()[0].timestamp == 66);
  EXPECT(account.spends()[0].unlock_time == 1500);
  EXPECT(account.spends()[0].mixin_count == 16);
  EXPECT(account.spends()[0].length == 32);
  EXPECT(account.spends()[0].payment_id == payment_id);
  EXPECT(account.spends()[0].sender.maj_i == lws::db::major_index(4));
  EXPECT(account.spends()[0].sender.min_i == lws::db::minor_index(55));

  lws::account copy{};
  EXPECT(!copy);

  {
    wire::msgpack_slice_writer dest{true};
    wire_write::bytes(dest, account);
    EXPECT(!wire::msgpack::from_bytes(epee::byte_slice{dest.take_sink()}, copy));
  }

  EXPECT(copy);
  EXPECT(copy.address() == account_address);
  EXPECT(copy.id() == db_account.id);
  EXPECT(copy.view_public() == db_account.address.view_public);
  EXPECT(copy.spend_public() == db_account.address.spend_public);
  EXPECT(copy.view_key() == keys.m_view_secret_key);
  EXPECT(copy.scan_height() == db_account.scan_height);
  {
    const auto result = copy.get_spendable(lws::db::output_id{100, 2000});
    EXPECT(bool(result));
    EXPECT(result->maj_i == lws::db::major_index(1));
    EXPECT(result->min_i == lws::db::minor_index(34));
  }
  EXPECT(copy.outputs().size() == 1);
  EXPECT(copy.outputs()[0].link == link);
  EXPECT(copy.outputs()[0].spend_meta.id.high == 500);
  EXPECT(copy.outputs()[0].spend_meta.id.low == 30);
  EXPECT(copy.outputs()[0].spend_meta.amount == 40000);
  EXPECT(copy.outputs()[0].spend_meta.mixin_count == 16);
  EXPECT(copy.outputs()[0].spend_meta.index == 2);
  EXPECT(copy.outputs()[0].spend_meta.tx_public == tx_public);
  EXPECT(copy.outputs()[0].timestamp == 7000);
  EXPECT(copy.outputs()[0].unlock_time == 4670);
  EXPECT(copy.outputs()[0].tx_prefix_hash == tx_prefix);
  EXPECT(copy.outputs()[0].pub == pub);
  EXPECT(copy.outputs()[0].ringct_mask == ringct);
  {
    const auto unpacked = lws::db::unpack(copy.outputs()[0].extra);
    EXPECT(unpacked.first == extra);
    EXPECT(unpacked.second == sizeof(crypto::hash));
  }
  EXPECT(copy.outputs()[0].recipient.maj_i == lws::db::major_index(2));
  EXPECT(copy.outputs()[0].recipient.min_i == lws::db::minor_index(66));

  EXPECT(copy.spends().size() == 1);
  EXPECT(copy.spends()[0].link == link);
  EXPECT(copy.spends()[0].image == image);
  EXPECT(copy.spends()[0].source.high == 10);
  EXPECT(copy.spends()[0].source.low == 70000);
  EXPECT(copy.spends()[0].timestamp == 66);
  EXPECT(copy.spends()[0].unlock_time == 1500);
  EXPECT(copy.spends()[0].mixin_count == 16);
  EXPECT(copy.spends()[0].length == 32);
  EXPECT(copy.spends()[0].payment_id == payment_id);
  EXPECT(copy.spends()[0].sender.maj_i == lws::db::major_index(4));
  EXPECT(copy.spends()[0].sender.min_i == lws::db::minor_index(55));
}
