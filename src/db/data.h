  // Copyright (c) 2018-2020, The Monero Project
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

#include <boost/uuid/uuid.hpp>
#include <cassert>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <string>
#include <utility>

#include "crypto/crypto.h"
#include "lmdb/util.h"
#include "ringct/rctTypes.h" //! \TODO brings in lots of includes, try to remove
#include "wire/fwd.h"
#include "wire/json/fwd.h"
#include "wire/msgpack/fwd.h"
#include "wire/traits.h"

namespace lws
{
namespace db
{
  /*
    Enum classes are used because they generate identical code to native integer
    types, but are not implicitly convertible to each other or any integer types.
    They also have comparison but not arithmetic operators defined.
  */

  //! References an account stored in the database, faster than by address
  enum class account_id : std::uint32_t
  {
    invalid = std::uint32_t(-1) //!< Always represents _not an_ account id.
  };
  WIRE_AS_INTEGER(account_id);

  //! Number of seconds since UNIX epoch.
  enum class account_time : std::uint32_t {};
  WIRE_AS_INTEGER(account_time);

  //! References a block height
  enum class block_id : std::uint64_t
  {
    txpool = std::uint64_t(-1) //! Represents not-yet-a-block
  };
  WIRE_AS_INTEGER(block_id);

  //! References a global output number, as determined by the public chain
  struct output_id
  {
    //! \return Special ID for outputs not yet in a block.
    static constexpr output_id txpool() noexcept
    { return {0, std::numeric_limits<std::uint64_t>::max()}; }

    std::uint64_t high; //!< Amount on public chain; rct outputs are `0`
    std::uint64_t low;  //!< Offset within `amount` on the public chain
  };
  WIRE_DECLARE_OBJECT(output_id);

  enum class account_status : std::uint8_t
  {
    active = 0, //!< Actively being scanned and reported by API
    inactive,   //!< Not being scanned, but still reported by API
    hidden      //!< Not being scanned or reported by API
  };
  WIRE_DECLARE_ENUM(account_status);

  enum account_flags : std::uint8_t
  {
    default_account = 0,
    admin_account   = 1,          //!< Indicates `key` can be used for admin requests
    account_generated_locally = 2 //!< Flag sent by client on initial login request
  };

  enum class request : std::uint8_t
  {
    create = 0, //!< Add a new account
    import_scan //!< Set account start and scan height to zero.
  };
  WIRE_DECLARE_ENUM(request);

  /*!
    DB does not use `crypto::secret_key` because it is not POD (UB to copy over
    entire struct). LMDB is keeping a copy in process memory anyway (row
    encryption not currently used). The roadmap recommends process isolation
    per-connection by default as a defense against viewkey leaks due to bug. */

  struct view_key : crypto::ec_scalar {};
  // wire::is_blob trait below

  //! The public keys of a monero address
  struct account_address
  {
    crypto::public_key view_public; //!< Must be first for LMDB optimizations.
    crypto::public_key spend_public;
  };
  static_assert(sizeof(account_address) == 64, "padding in account_address");
  WIRE_DECLARE_OBJECT(account_address);

  struct account
  {
    account_id id;          //!< Must be first for LMDB optimizations
    account_time access;    //!< Last time `get_address_info` was called.
    account_address address;
    view_key key;           //!< Doubles as authorization handle for REST API.
    block_id scan_height;   //!< Last block scanned; check-ins are always by block
    block_id start_height;  //!< Account started scanning at this block height
    account_time creation;  //!< Time account first appeared in database.
    account_flags flags;    //!< Additional account info bitmask.
    char reserved[3];
  };
  static_assert(sizeof(account) == (4 * 2) + 64 + 32 + (8 * 2) + (4 * 2), "padding in account");
  void write_bytes(wire::writer&, const account&, bool show_key = false);

  struct block_info
  {
    block_id id;      //!< Must be first for LMDB optimizations
    crypto::hash hash;
  };
  static_assert(sizeof(block_info) == 8 + 32, "padding in block_info");
  WIRE_DECLARE_OBJECT(block_info);

  //! `output`s and `spend`s are sorted by these fields to make merging easier.
  struct transaction_link
  {
    block_id height;      //!< Block height containing transaction
    crypto::hash tx_hash; //!< Hash of the transaction
  };

  //! Additional flags stored in `output`s.
  enum extra : std::uint8_t
  {
    coinbase_output = 1,
    ringct_output   = 2
  };

  //! Packed information stored in `output`s.
  enum class extra_and_length : std::uint8_t {};

  //! \return `val` and `length` packed into a single byte.
  inline extra_and_length pack(extra val, std::uint8_t length) noexcept
  {
    assert(length <= 32);
    return extra_and_length((std::uint8_t(val) << 6) | (length & 0x3f));
  }

  //! \return `extra` and length unpacked from a single byte.
  inline std::pair<extra, std::uint8_t> unpack(extra_and_length val) noexcept
  {
    const std::uint8_t real_val = std::uint8_t(val);
    return {extra(real_val >> 6), std::uint8_t(real_val & 0x3f)};
  }

  //! Information for an output that has been received by an `account`.
  struct output
  {
    transaction_link link;        //! Orders and links `output` to `spend`s.

    //! Data that a linked `spend` needs in some REST endpoints.
    struct spend_meta_
    {
      output_id id;             //!< Unique id for output within monero
      // `link` and `id` must be in this order for LMDB optimizations
      std::uint64_t amount;
      std::uint32_t mixin_count;//!< Ring-size of TX
      std::uint32_t index;      //!< Offset within a tx
      crypto::public_key tx_public;
    } spend_meta;

    std::uint64_t timestamp;
    std::uint64_t unlock_time; //!< Not always a timestamp; mirrors chain value.
    crypto::hash tx_prefix_hash;
    crypto::public_key pub;    //!< One-time spendable public key.
    rct::key ringct_mask;      //!< Unencrypted CT mask
    char reserved[7];
    extra_and_length extra;    //!< Extra info + length of payment id
    union payment_id_
    {
      crypto::hash8 short_;  //!< Decrypted short payment id
      crypto::hash long_;    //!< Long version of payment id (always decrypted)
    } payment_id;
    std::uint64_t fee;       //!< Total fee for transaction
  };
  static_assert(
    sizeof(output) == 8 + 32 + (8 * 3) + (4 * 2) + 32 + (8 * 2) + (32 * 3) + 7 + 1 + 32 + 8,
    "padding in output"
  );
  void write_bytes(wire::writer&, const output&);

  //! Information about a possible spend of a received `output`.
  struct spend
  {
    transaction_link link;    //!< Orders and links `spend` to `output`.
    crypto::key_image image;  //!< Unique ID for the spend
    // `link` and `image` must in this order for LMDB optimizations
    output_id source;         //!< The output being spent
    std::uint64_t timestamp;  //!< Timestamp of spend
    std::uint64_t unlock_time;//!< Unlock time of spend
    std::uint32_t mixin_count;//!< Ring-size of TX output
    char reserved[3];
    std::uint8_t length;      //!< Length of `payment_id` field (0..32).
    crypto::hash payment_id;  //!< Unencrypted only, can't decrypt spend
  };
  static_assert(sizeof(spend) == 8 + 32 * 2 + 8 * 4 + 4 + 3 + 1 + 32, "padding in spend");
  WIRE_DECLARE_OBJECT(spend);

  //! Key image and info needed to retrieve primary `spend` data.
  struct key_image
  {
    crypto::key_image value; //!< Actual key image value
    // The above field needs to be first for LMDB optimizations
    transaction_link link;   //!< Link to `spend` and `output`.
  };
  WIRE_DECLARE_OBJECT(key_image);

  struct request_info
  {
    account_address address;//!< Must be first for LMDB optimizations
    view_key key;
    block_id start_height;
    account_time creation;        //!< Time the request was created.
    account_flags creation_flags; //!< Generated locally?
    char reserved[3];
  };
  static_assert(sizeof(request_info) == 64 + 32 + 8 + (4 * 2), "padding in request_info");
  void write_bytes(wire::writer& dest, const request_info& self, bool show_key = false);

  enum class webhook_type : std::uint8_t
  {
    tx_confirmation = 0,
    // unconfirmed_tx,
    // new_block
    // confirmed_tx,
    // double_spend_tx,
    // tx_confidence
  };
  WIRE_DECLARE_ENUM(webhook_type);

  //! Key for upcoming webhooks or in-progress webhooks
  struct webhook_key
  {
    account_id user;
    webhook_type type;
    char reserved[3];
  };
  static_assert(sizeof(webhook_key) == 4 + 1 + 3, "padding in webhook_key");
  WIRE_DECLARE_OBJECT(webhook_key);

  //! Webhook values used to sort by duplicate keys
  struct webhook_dupsort
  {
    std::uint64_t payment_id; //!< Only used with `tx_confirmation` type.
    boost::uuids::uuid event_id;
  };
  static_assert(sizeof(webhook_dupsort) == 8 + 16, "padding in webhoook");

  //! Variable length data for a webhook key/event
  struct webhook_data
  {
    std::string url;
    std::string token;
    std::uint32_t confirmations;
  };
  WIRE_MSGPACK_DECLARE_OBJECT(webhook_data);

  //! Compatible with lmdb::table code
  using webhook_value = std::pair<webhook_dupsort, webhook_data>;
  WIRE_DECLARE_OBJECT(webhook_value);

  //! Returned by DB when a webhook event "tripped"
  struct webhook_tx_confirmation
  {
    webhook_key key;
    webhook_value value;
    output tx_info;
  };
  void write_bytes(wire::writer&, const webhook_tx_confirmation&);

  //! References a specific output that triggered a webhook
  struct webhook_output
  {
    transaction_link tx;
    output_id out;
  };

  //! References all info from a webhook that triggered
  struct webhook_event
  {
    webhook_output link;
    webhook_dupsort link_webhook;
  };
  void write_bytes(wire::json_writer&, const webhook_event&);

  bool operator==(transaction_link const& left, transaction_link const& right) noexcept;
  bool operator<(transaction_link const& left, transaction_link const& right) noexcept;
  bool operator<=(transaction_link const& left, transaction_link const& right) noexcept;

  inline constexpr bool operator==(output_id left, output_id right) noexcept
  {
    return left.high == right.high && left.low == right.low;
  }
  inline constexpr bool operator!=(output_id left, output_id right) noexcept
  {
    return left.high != right.high || left.low != right.low;
  }
  inline constexpr bool operator<(output_id left, output_id right) noexcept
  {
    return left.high == right.high ?
      left.low < right.low : left.high < right.high;
  }
  inline constexpr bool operator<=(output_id left, output_id right) noexcept
  {
    return left.high == right.high ?
      left.low <= right.low : left.high < right.high;
  }
  inline constexpr bool operator<(const webhook_key& left, const webhook_key& right) noexcept
  {
    return left.user == right.user ?
      left.type < right.type : left.user < right.user;
  }

  bool operator<(const webhook_dupsort& left, const webhook_dupsort& right) noexcept;

  inline bool operator==(const webhook_output& left, const webhook_output& right) noexcept
  {
    return left.out == right.out && left.tx == right.tx;
  }
  inline bool operator<(const webhook_output& left, const webhook_output& right) noexcept
  {
    return left.tx == right.tx ? left.out < right.out : left.tx < right.tx;
  }
  inline bool operator<(const webhook_event& left, const webhook_event& right) noexcept
  {
    return left.link == right.link ?
      left.link_webhook < right.link_webhook : left.link < right.link;
  }


  /*!
    Write `address` to `out` in base58 format using `lws::config::network` to
    determine tag. */
  std::ostream& operator<<(std::ostream& out, account_address const& address);
} // db
} // lws

namespace wire
{
  template<>
   struct is_blob<lws::db::view_key>
    : std::true_type
  {};
}
