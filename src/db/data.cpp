// Copyright (c) 2018, The Monero Project
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
#include "data.h"

#include <cstring>
#include <memory>

#include "cryptonote_config.h" // monero/src
#include "db/string.h"
#include "int-util.h"          // monero/contribe/epee/include
#include "ringct/rctOps.h"     // monero/src
#include "ringct/rctTypes.h"   // monero/src
#include "wire.h"
#include "wire/adapted/array.h"
#include "wire/crypto.h"
#include "wire/json/write.h"
#include "wire/msgpack.h"
#include "wire/uuid.h"
#include "wire/vector.h"
#include "wire/wrapper/defaulted.h"

namespace lws
{
namespace db
{
  namespace
  {
    template<typename F, typename T>
    void map_output_id(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD(high), WIRE_FIELD(low));
    }
  }
  WIRE_DEFINE_OBJECT(output_id, map_output_id);

  namespace
  {
    constexpr const char* map_account_status[] = {"active", "inactive", "hidden"};
    constexpr const char* map_request[] = {"create", "import"};
  }
  WIRE_DEFINE_ENUM(account_status, map_account_status);
  WIRE_DEFINE_ENUM(request, map_request);

  namespace
  {
    template<typename F, typename T>
    void map_account_address(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD(spend_public), WIRE_FIELD(view_public));
    }
  }
  WIRE_DEFINE_OBJECT(account_address, map_account_address);

  namespace
  {
    template<typename F, typename T>
    void map_subaddress_dict(F& format, T& self)
    {
      wire::object(format,
        wire::field<0>("key", std::ref(self.first)),
        wire::field<1>("value", std::ref(self.second))
      );
    }
  }

  bool check_subaddress_dict(const subaddress_dict& self)
  {
    bool is_first = true;
    minor_index last = minor_index::primary;
    for (const auto& elem : self.second)
    {
      if (elem[1] < elem[0])
      {
        MERROR("Invalid subaddress_range (last before first");
        return false;
      }
      if (std::uint32_t(elem[0]) <= std::uint64_t(last) + 1 && !is_first)
      {
        MERROR("Invalid subaddress_range (overlapping with previous)");
        return false;
      }
      is_first = false;
      last = elem[1];
    }
    return true;
  }
  void read_bytes(wire::reader& source, subaddress_dict& dest)
  {
    map_subaddress_dict(source, dest);
    if (!check_subaddress_dict(dest))
      WIRE_DLOG_THROW_(wire::error::schema::array);
  }
  void write_bytes(wire::writer& dest, const subaddress_dict& source)
  { 
    if (!check_subaddress_dict(source))
      WIRE_DLOG_THROW_(wire::error::schema::array);
    map_subaddress_dict(dest, source);
  }

  namespace
  {
    template<typename F, typename T>
    void map_address_index(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD_ID(0, maj_i), WIRE_FIELD_ID(1, min_i));
    }

    crypto::secret_key get_subaddress_secret_key(const crypto::secret_key &a, const std::uint32_t major, const std::uint32_t minor)
    {
      char data[sizeof(config::HASH_KEY_SUBADDRESS) + sizeof(crypto::secret_key) + 2 * sizeof(uint32_t)];
      memcpy(data, config::HASH_KEY_SUBADDRESS, sizeof(config::HASH_KEY_SUBADDRESS));
      memcpy(data + sizeof(config::HASH_KEY_SUBADDRESS), &a, sizeof(crypto::secret_key));
      std::uint32_t idx = SWAP32LE(major);
      memcpy(data + sizeof(config::HASH_KEY_SUBADDRESS) + sizeof(crypto::secret_key), &idx, sizeof(uint32_t));
      idx = SWAP32LE(minor);
      memcpy(data + sizeof(config::HASH_KEY_SUBADDRESS) + sizeof(crypto::secret_key) + sizeof(uint32_t), &idx, sizeof(uint32_t));
      crypto::secret_key m;
      crypto::hash_to_scalar(data, sizeof(data), m);
      return m;
    }
  }
  WIRE_DEFINE_OBJECT(address_index, map_address_index);

  crypto::public_key address_index::get_spend_public(account_address const& base, crypto::secret_key const& view) const
  {
    if (is_zero())
      return base.spend_public;

    // m = Hs(a || index_major || index_minor)
    crypto::secret_key m = get_subaddress_secret_key(view, std::uint32_t(maj_i), std::uint32_t(min_i));

    // M = m*G
    crypto::public_key M;
    crypto::secret_key_to_public_key(m, M);

    // D = B + M
    return rct::rct2pk(rct::addKeys(rct::pk2rct(base.spend_public), rct::pk2rct(M)));
  }

  namespace
  {
    template<typename F, typename T>
    void map_subaddress_map(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD_ID(0, subaddress), WIRE_FIELD_ID(1, index));
    }
  }
  WIRE_DEFINE_OBJECT(subaddress_map, map_subaddress_map);

  void write_bytes(wire::writer& dest, const account& self, const bool show_key)
  {
    view_key const* const key =
      show_key ? std::addressof(self.key) : nullptr;
    const bool admin = (self.flags & admin_account);
    const bool generated_locally = (self.flags & account_generated_locally);

    wire::object(dest,
      WIRE_FIELD(id),
      wire::field("access_time", self.access),
      WIRE_FIELD(address),
      wire::optional_field("view_key", key),
      WIRE_FIELD(scan_height),
      WIRE_FIELD(start_height),
      wire::field("creation_time", self.creation),
      wire::field("admin", admin),
      wire::field("generated_locally", generated_locally)
    );
  }

  namespace
  {
    template<typename F, typename T>
    void map_block_info(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD(id), WIRE_FIELD(hash));
    }
  }
  WIRE_DEFINE_OBJECT(block_info, map_block_info);

  namespace
  {
    template<typename F, typename T>
    void map_transaction_link(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD_ID(0, height), WIRE_FIELD_ID(1, tx_hash));
    }
  }
  WIRE_DEFINE_OBJECT(transaction_link, map_transaction_link);

  void write_bytes(wire::writer& dest, const output& self)
  {
    const std::pair<db::extra, std::uint8_t> unpacked =
      db::unpack(self.extra);

    const bool coinbase = (unpacked.first & lws::db::coinbase_output);
    const bool rct = (unpacked.first & lws::db::ringct_output);

    const auto rct_mask = rct ? std::addressof(self.ringct_mask) : nullptr;

    epee::span<const std::uint8_t> payment_bytes{};
    if (unpacked.second == 32)
      payment_bytes = epee::as_byte_span(self.payment_id.long_);
    else if (unpacked.second == 8)
      payment_bytes = epee::as_byte_span(self.payment_id.short_);

    const auto payment_id = payment_bytes.empty() ?
      nullptr : std::addressof(payment_bytes);

    // defaulted will omit "id" and "block" when the output is in the
    // txpool with no valid values.
    wire::object(dest,
      wire::optional_field<0>("id", wire::defaulted(std::cref(self.spend_meta.id), output_id::txpool())),
      wire::optional_field<1>("block", wire::defaulted(self.link.height, block_id::txpool)),
      wire::field<2>("index", self.spend_meta.index),
      wire::field<3>("amount", self.spend_meta.amount),
      wire::field<4>("timestamp", self.timestamp),
      wire::field<5>("tx_hash", std::cref(self.link.tx_hash)),
      wire::field<6>("tx_prefix_hash", std::cref(self.tx_prefix_hash)),
      wire::field<7>("tx_public", std::cref(self.spend_meta.tx_public)),
      wire::optional_field<8>("rct_mask", rct_mask),
      wire::optional_field<9>("payment_id", payment_id),
      wire::field<10>("unlock_time", self.unlock_time),
      wire::field<11>("mixin_count", self.spend_meta.mixin_count),
      wire::field<12>("coinbase", coinbase),
      wire::field<13>("fee", self.fee),
      wire::field<14>("recipient", self.recipient)
    );
  }

  namespace
  {
    template<typename F, typename T1, typename T2>
    void map_spend(F& format, T1& self, T2& payment_id)
    {
      wire::object(format,
        wire::field("height", std::ref(self.link.height)),
        wire::field("tx_hash", std::ref(self.link.tx_hash)),
        WIRE_FIELD(image),
        WIRE_FIELD(source),
        WIRE_FIELD(timestamp),
        WIRE_FIELD(unlock_time),
        WIRE_FIELD(mixin_count),
        wire::optional_field("payment_id", std::ref(payment_id)),
        WIRE_FIELD(sender)
      );
    }
  }
  void read_bytes(wire::reader& source, spend& dest)
  {
    boost::optional<crypto::hash> payment_id;
    map_spend(source, dest, payment_id);

    if (payment_id)
    {
      dest.length = sizeof(dest.payment_id);
      dest.payment_id = std::move(*payment_id);
    }
    else
      dest.length = 0;
  }
  void write_bytes(wire::writer& dest, const spend& source)
  {
    crypto::hash const* const payment_id =
      (source.length == sizeof(source.payment_id) ?
        std::addressof(source.payment_id) : nullptr);
    return map_spend(dest, source, payment_id);
  }

  namespace
  {
    template<typename F, typename T>
    void map_key_image(F& format, T& self)
    {
      wire::object(format,
        wire::field("key_image", std::ref(self.value)),
        wire::field("tx_hash", std::ref(self.link.tx_hash)),
        wire::field("height", std::ref(self.link.height))
      );
    }
  }
  WIRE_DEFINE_OBJECT(key_image, map_key_image);

  void write_bytes(wire::writer& dest, const request_info& self, const bool show_key)
  {
    db::view_key const* const key =
      show_key ? std::addressof(self.key) : nullptr;
    const bool generated = (self.creation_flags & lws::db::account_generated_locally);

    wire::object(dest,
      WIRE_FIELD(address),
      wire::optional_field("view_key", key),
      WIRE_FIELD(start_height),
      wire::field("generated_locally", generated)
    );
  }

  namespace
  {
    constexpr const char* map_webhook_type[] = {"tx-confirmation", "new-account"};

    template<typename F, typename T>
    void map_webhook_key(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD_ID(0, user), WIRE_FIELD_ID(1, type));
    }

    template<typename F, typename T>
    void map_webhook_data(F& format, T& self)
    {
      wire::object(format,
        WIRE_FIELD_ID(0, url),
        WIRE_FIELD_ID(1, token),
        WIRE_FIELD_ID(2, confirmations)
      );
    }

    template<typename F, typename T>
    void map_webhook_value(F& format, T& self, crypto::hash8& payment_id)
    {
      static_assert(sizeof(payment_id) == sizeof(self.first.payment_id), "bad memcpy");
      wire::object(format,
        wire::field<0>("payment_id", std::ref(payment_id)),
        wire::field<1>("event_id", std::ref(self.first.event_id)),
        wire::field<2>("token", std::ref(self.second.token)),
        wire::field<3>("confirmations", self.second.confirmations),
        wire::field<4>("url", std::ref(self.second.url))
      );
    }
  }
  WIRE_DEFINE_ENUM(webhook_type, map_webhook_type);
  WIRE_DEFINE_OBJECT(webhook_key, map_webhook_key);
  WIRE_MSGPACK_DEFINE_OBJECT(webhook_data, map_webhook_data);

  void read_bytes(wire::reader& source, webhook_value& dest)
  {
    crypto::hash8 payment_id{};
    map_webhook_value(source, dest, payment_id);
    std::memcpy(std::addressof(dest.first.payment_id), std::addressof(payment_id), sizeof(payment_id));
  }
  void write_bytes(wire::writer& dest, const webhook_value& source)
  {
    crypto::hash8 payment_id;
    std::memcpy(std::addressof(payment_id), std::addressof(source.first.payment_id), sizeof(payment_id));
    map_webhook_value(dest, source, payment_id);
  }

  void write_bytes(wire::writer& dest, const webhook_tx_confirmation& self)
  {
    crypto::hash8 payment_id;
    static_assert(sizeof(payment_id) == sizeof(self.value.first.payment_id), "bad memcpy");
    std::memcpy(std::addressof(payment_id), std::addressof(self.value.first.payment_id), sizeof(payment_id));
    // to be sent to remote url
    wire::object(dest,
      wire::field<0>("event", std::cref(self.key.type)),
      wire::field<1>("payment_id", std::cref(payment_id)),
      wire::field<2>("token", std::cref(self.value.second.token)),
      wire::field<3>("confirmations", std::cref(self.value.second.confirmations)),
      wire::field<4>("event_id", std::cref(self.value.first.event_id)),
      WIRE_FIELD_ID(5, tx_info)
    );
  }

  void write_bytes(wire::json_writer& dest, const webhook_event& self)
  {
    crypto::hash8 payment_id;
    static_assert(sizeof(payment_id) == sizeof(self.link_webhook.payment_id), "bad memcpy");
    std::memcpy(std::addressof(payment_id), std::addressof(self.link_webhook.payment_id), sizeof(payment_id));
    wire::object(dest,
      wire::field<0>("tx_info", std::cref(self.link.tx)),
      wire::field<1>("output_id", std::cref(self.link.out)),
      wire::field<2>("payment_id", std::cref(payment_id)),
      wire::field<3>("event_id", std::cref(self.link_webhook.event_id))
    );
  }

  void write_bytes(wire::writer& dest, const webhook_new_account& self)
  {
    wire::object(dest,
      wire::field<0>("event_id", std::cref(self.value.first.event_id)),
      wire::field<1>("token", std::cref(self.value.second.token)),
      wire::field<2>("address", address_string(self.account))
    );
  }

  bool operator<(const webhook_dupsort& left, const webhook_dupsort& right) noexcept
  {
    return left.payment_id == right.payment_id ?
      std::memcmp(std::addressof(left.event_id), std::addressof(right.event_id), sizeof(left.event_id)) < 0 :
      left.payment_id < right.payment_id;
  }

  /*! TODO consider making an `operator<` for `crypto::tx_hash`. Not known to be
    needed elsewhere yet. */
  bool operator==(transaction_link const& left, transaction_link const& right) noexcept
  {
    return left.height == right.height &&
      std::memcmp(std::addressof(left.tx_hash), std::addressof(right.tx_hash), sizeof(left.tx_hash)) == 0;
  }
  bool operator<(transaction_link const& left, transaction_link const& right) noexcept
  {
    return left.height == right.height ?
      std::memcmp(std::addressof(left.tx_hash), std::addressof(right.tx_hash), sizeof(left.tx_hash)) < 0 :
      left.height < right.height;
  }
  bool operator<=(transaction_link const& left, transaction_link const& right) noexcept
  {
    return right.height == left.height ?
      std::memcmp(std::addressof(left.tx_hash), std::addressof(right.tx_hash), sizeof(left.tx_hash)) <= 0 :
      left.height < right.height;
  }
} // db
} // lws
