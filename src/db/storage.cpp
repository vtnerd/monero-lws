// Copyright (c) 2018-2023, The Monero Project
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
#include "storage.h"

#include <boost/container/static_vector.hpp>
#include <boost/core/demangle.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/counting_range.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/uuid/uuid_hash.hpp>
#include <cassert>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include "checkpoints/checkpoints.h"
#include "config.h"
#include "crypto/crypto.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "db/account.h"
#include "db/string.h"
#include "error.h"
#include "hex.h"
#include "lmdb/database.h"
#include "lmdb/error.h"
#include "lmdb/key_stream.h"
#include "lmdb/msgpack_table.h"
#include "lmdb/table.h"
#include "lmdb/util.h"
#include "lmdb/value_stream.h"
#include "net/net_parse_helpers.h" // monero/contrib/epee/include
#include "span.h"
#include "wire/adapted/array.h"
#include "wire/filters.h"
#include "wire/json.h"
#include "wire/vector.h"
#include "wire/wrapper/array.h"
#include "wire/wrappers_impl.h"

namespace wire
{
  template<typename T, typename C>
  static bool operator<(const array_<T, C>& lhs, const array_<T, C>& rhs)
  {
    return lhs.get_container() < rhs.get_container();
  }
}

namespace lws
{
namespace db
{
  namespace v0
  { 
    //! Orignal DB value, with no txn fee
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
    };
    static_assert(
      sizeof(output) == 8 + 32 + (8 * 3) + (4 * 2) + 32 + (8 * 2) + (32 * 3) + 7 + 1 + 32,
      "padding in output"
    );

    //! Original db value, with no subaddress
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
  }

  namespace v1
  {
    //! Second DB value, with no subaddress
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
  }


  namespace
  {
    //! Used for finding `account` instances by other indexes.
    struct account_lookup
    {
      account_id id;
      account_status status;
      char reserved[3];
    };
    static_assert(sizeof(account_lookup) == 4 + 1 + 3, "padding in account_lookup");

    //! Used for looking up accounts by their public address.
    struct account_by_address
    {
      account_address address; //!< Must be first for LMDB optimizations
      account_lookup lookup;
    };
    static_assert(sizeof(account_by_address) == 64 + 4 + 1 + 3, "padding in account_by_address");

    constexpr const unsigned blocks_version = 0;
    constexpr const unsigned by_address_version = 0;
    constexpr const unsigned pows_version = 0;

    template<typename T>
    int less(epee::span<const std::uint8_t> left, epee::span<const std::uint8_t> right) noexcept
    {
      if (left.size() < sizeof(T))
      {
        assert(left.empty());
        return -1;
      }
      if (right.size() < sizeof(T))
      {
        assert(right.empty());
        return 1;
      }

      T left_val;
      T right_val;
      std::memcpy(std::addressof(left_val), left.data(), sizeof(T));
      std::memcpy(std::addressof(right_val), right.data(), sizeof(T));

      return (left_val < right_val) ? -1 : int(right_val < left_val);
    }

    int compare_32bytes(epee::span<const std::uint8_t> left, epee::span<const std::uint8_t> right) noexcept
    {
      if (left.size() < 32)
      {
        assert(left.empty());
        return -1;
      }
      if (right.size() < 32)
      {
        assert(right.empty());
        return 1;
      }

      return std::memcmp(left.data(), right.data(), 32);
    }

    int output_compare(MDB_val const* left, MDB_val const* right) noexcept
    {
      if (left == nullptr || right == nullptr)
      {
        assert("MDB_val nullptr" == 0);
        return -1;
      }

      auto left_bytes = lmdb::to_byte_span(*left);
      auto right_bytes = lmdb::to_byte_span(*right);

      int diff = less<lmdb::native_type<block_id>>(left_bytes, right_bytes);
      if (diff)
        return diff;

      left_bytes.remove_prefix(sizeof(block_id));
      right_bytes.remove_prefix(sizeof(block_id));

      static_assert(sizeof(crypto::hash) == 32, "bad memcmp below");
      diff = compare_32bytes(left_bytes, right_bytes);
      if (diff)
        return diff;

      left_bytes.remove_prefix(sizeof(crypto::hash));
      right_bytes.remove_prefix(sizeof(crypto::hash));
      return less<output_id>(left_bytes, right_bytes);
    }

    int spend_compare(MDB_val const* left, MDB_val const* right) noexcept
    {
      if (left == nullptr || right == nullptr)
      {
        assert("MDB_val nullptr" == 0);
        return -1;
      }

      auto left_bytes = lmdb::to_byte_span(*left);
      auto right_bytes = lmdb::to_byte_span(*right);

      int diff = less<lmdb::native_type<block_id>>(left_bytes, right_bytes);
      if (diff)
        return diff;

      left_bytes.remove_prefix(sizeof(block_id));
      right_bytes.remove_prefix(sizeof(block_id));

      static_assert(sizeof(crypto::hash) == 32, "bad memcmp below");
      diff = compare_32bytes(left_bytes, right_bytes);
      if (diff)
        return diff;

      left_bytes.remove_prefix(sizeof(crypto::hash));
      right_bytes.remove_prefix(sizeof(crypto::hash));

      static_assert(sizeof(crypto::key_image) == 32, "bad memcmp below");
      diff = compare_32bytes(left_bytes, right_bytes);
      if (diff)
        return diff;

      left_bytes.remove_prefix(sizeof(crypto::key_image));
      right_bytes.remove_prefix(sizeof(crypto::key_image));
      return less<output_id>(left_bytes, right_bytes);
    }

    constexpr const lmdb::basic_table<unsigned, block_info> blocks{
      "blocks_by_id", (MDB_CREATE | MDB_DUPSORT), MONERO_SORT_BY(block_info, id)
    };
    constexpr const lmdb::basic_table<unsigned, block_pow> pows{
      "pow_by_id", (MDB_CREATE | MDB_DUPSORT), MONERO_SORT_BY(block_pow, id)
    };
    constexpr const lmdb::basic_table<account_status, account> accounts{
      "accounts_by_status,id", (MDB_CREATE | MDB_DUPSORT), MONERO_SORT_BY(account, id)
    };
    constexpr const lmdb::basic_table<unsigned, account_by_address> accounts_by_address(
      "accounts_by_address", (MDB_CREATE | MDB_DUPSORT), MONERO_COMPARE(account_by_address, address.view_public)
    );
    constexpr const lmdb::basic_table<block_id, account_lookup> accounts_by_height(
      "accounts_by_height,id", (MDB_CREATE | MDB_DUPSORT), MONERO_SORT_BY(account_lookup, id)
    );
    constexpr const lmdb::basic_table<account_id, v0::output> outputs_v0{
      "outputs_by_account_id,block_id,tx_hash,output_id", MDB_DUPSORT, &output_compare
    };
    constexpr const lmdb::basic_table<account_id, v1::output> outputs_v1{
      "outputs_v1_by_account_id,block_id,tx_hash,output_id", MDB_DUPSORT, &output_compare
    };
    constexpr const lmdb::basic_table<account_id, output> outputs{
      "outputs_v2_by_account_id,block_id,tx_hash,output_id", (MDB_CREATE | MDB_DUPSORT), &output_compare
    };
    constexpr const lmdb::basic_table<account_id, v0::spend> spends_v0{
      "spends_by_account_id,block_id,tx_hash,image", MDB_DUPSORT, &spend_compare
    };
    constexpr const lmdb::basic_table<account_id, spend> spends{
      "spends_v1_by_account_id,block_id,tx_hash,image", (MDB_CREATE | MDB_DUPSORT), &spend_compare
    };
    constexpr const lmdb::basic_table<output_id, db::key_image> images{
      "key_images_by_output_id,image", (MDB_CREATE | MDB_DUPSORT), MONERO_COMPARE(db::key_image, value)
    };
    constexpr const lmdb::basic_table<request, request_info> requests{
      "requests_by_type,address", (MDB_CREATE | MDB_DUPSORT), MONERO_COMPARE(request_info, address.spend_public)
    };
    constexpr const lmdb::msgpack_table<webhook_key, webhook_dupsort, webhook_data> webhooks{
      "webhooks_by_account_id,payment_id", (MDB_CREATE | MDB_DUPSORT), &lmdb::less<db::webhook_dupsort>
    };
    constexpr const lmdb::basic_table<account_id, webhook_event> events_by_account_id{
      "webhook_events_by_account_id,type,block_id,tx_hash,output_id,payment_id,event_id", (MDB_CREATE | MDB_DUPSORT), &lmdb::less<webhook_event>
    };
    constexpr const lmdb::msgpack_table<account_id, major_index, index_ranges> subaddress_ranges{
      "subaddress_ranges_by_account_id,major_index", (MDB_CREATE | MDB_DUPSORT), &lmdb::less<db::major_index>
    };
    constexpr const lmdb::basic_table<account_id, subaddress_map> subaddress_indexes{
      "subaddress_indexes_by_account_id,public_key", (MDB_CREATE | MDB_DUPSORT), MONERO_COMPARE(subaddress_map, subaddress)
    };

    template<typename D>
    expect<void> check_cursor(MDB_txn& txn, MDB_dbi tbl, std::unique_ptr<MDB_cursor, D>& cur) noexcept
    {
      if (cur)
      {
        MONERO_LMDB_CHECK(mdb_cursor_renew(&txn, cur.get()));
      }
      else
      {
        auto new_cur = lmdb::open_cursor<D>(txn, tbl);
        if (!new_cur)
          return new_cur.error();
        cur = std::move(*new_cur);
      }
      return success();
    }

    template<typename K, typename V>
    expect<void> bulk_insert(MDB_cursor& cur, K const& key, epee::span<V> values, unsigned flags = MDB_NODUPDATA) noexcept
    {
      while (!values.empty())
      {
        void const* const data = reinterpret_cast<void const*>(values.data());
        MDB_val key_bytes = lmdb::to_val(key);
        MDB_val value_bytes[2] = {
          MDB_val{sizeof(V), const_cast<void*>(data)}, MDB_val{values.size(), nullptr}
        };

        int err = mdb_cursor_put(
          &cur, &key_bytes, value_bytes, (flags | MDB_MULTIPLE)
        );
        if (err && err != MDB_KEYEXIST)
          return {lmdb::error(err)};

        values.remove_prefix(value_bytes[1].mv_size + (err == MDB_KEYEXIST ? 1 : 0));
      }
      return success();
    }

    //! Convert table to new format, then delete old table
    template<typename X, typename Y>
    expect<void> convert_table(MDB_txn& txn, MDB_dbi old, MDB_dbi current)
    {
      MINFO("DB update: " + boost::core::demangle(typeid(X).name()) + " to " + boost::core::demangle(typeid(Y).name()));

      cursor::outputs old_cur;
      cursor::outputs current_cur;
      MONERO_CHECK(check_cursor(txn, old, old_cur));
      MONERO_CHECK(check_cursor(txn, current, current_cur));

      MDB_val key{};
      MDB_val value{};
      int err = mdb_cursor_get(old_cur.get(), &key, &value, MDB_FIRST);
      for (;;)
      {
        if (err)
        {
          if (err == MDB_NOTFOUND)
          {
            // Remove old table entirely
            MONERO_LMDB_CHECK(mdb_drop(&txn, old, 1));
            return success();
          }
          return {lmdb::error(err)};
        }

        static_assert(sizeof(Y) >= sizeof(X), "unexpected sizeof");
        if (sizeof(X) != value.mv_size)
          return {lmdb::error(MDB_CORRUPTED)};

        Y transition{};
        std::memcpy(std::addressof(transition), value.mv_data, value.mv_size);

        value = lmdb::to_val(transition);
        MONERO_LMDB_CHECK(mdb_cursor_put(current_cur.get(), &key, &value, 0));
        err = mdb_cursor_get(old_cur.get(), &key, &value, MDB_NEXT);
      }
    }

    //! \return Current block hash at `id` using `cur`.
    expect<crypto::hash> do_get_block_hash(MDB_cursor& cur, block_id id) noexcept
    {
      MDB_val key = lmdb::to_val(blocks_version);
      MDB_val value = lmdb::to_val(id);
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_GET_BOTH));
      return blocks.get_value<MONERO_FIELD(block_info, hash)>(value);
    }

    void check_blockchain(MDB_txn& txn, MDB_dbi tbl)
    {
      cursor::blocks cur = MONERO_UNWRAP(lmdb::open_cursor<cursor::close_blocks>(txn, tbl));

      std::map<std::uint64_t, crypto::hash> const& points =
        storage::get_checkpoints().get_points();

      if (points.empty() || points.begin()->first != 0)
        MONERO_THROW(lws::error::bad_blockchain, "Checkpoints are empty/expected genesis hash");

      MDB_val key = lmdb::to_val(blocks_version);
      int err = mdb_cursor_get(cur.get(), &key, nullptr, MDB_SET);
      if (err)
      {
        if (err != MDB_NOTFOUND)
          MONERO_THROW(lmdb::error(err), "Unable to retrieve blockchain hashes");

        // new database
        block_info checkpoint{
          block_id(points.begin()->first), points.begin()->second
        };

        MDB_val value = lmdb::to_val(checkpoint);
        err = mdb_cursor_put(cur.get(), &key, &value, MDB_NODUPDATA);
        if (err)
          MONERO_THROW(lmdb::error(err), "Unable to add hash to local blockchain");

        if (1 < points.size())
        {
          checkpoint = block_info{
            block_id(points.rbegin()->first), points.rbegin()->second
          };

          value = lmdb::to_val(checkpoint);
          err = mdb_cursor_put(cur.get(), &key, &value, MDB_NODUPDATA);
          if (err)
            MONERO_THROW(lmdb::error(err), "Unable to add hash to local blockchain");
        }
      }
      else // inspect existing database
      {
        ///
        /// TODO Trim blockchain after a checkpoint has been reached
        ///
        const crypto::hash genesis = MONERO_UNWRAP(do_get_block_hash(*cur, block_id(0)));
        if (genesis != points.begin()->second)
        {
          MONERO_THROW(
            lws::error::bad_blockchain, "Genesis hash mismatch"
          );
        }
      }
    }

    void check_pow(MDB_txn& txn, MDB_dbi tbl)
    {
      cursor::pow cur = MONERO_UNWRAP(lmdb::open_cursor<cursor::close_pow>(txn, tbl));

      MDB_val key = lmdb::to_val(pows_version);
      int err = mdb_cursor_get(cur.get(), &key, nullptr, MDB_SET);
      if (err)
      {
        if (err != MDB_NOTFOUND)
          MONERO_THROW(lmdb::error(err), "Unable to retrieve blockchain hashes");

        // new database
        block_pow checkpoint{block_id(0), 0u, block_difficulty{0u, 1u}};
        MDB_val value = lmdb::to_val(checkpoint);
        err = mdb_cursor_put(cur.get(), &key, &value, MDB_NODUPDATA);
        if (err)
          MONERO_THROW(lmdb::error(err), "Unable to add hash to local blockchain");
      }
    }

    template<typename T>
    expect<void> get_blocks_tail(T& out, MDB_cursor& cur, MDB_val value, std::size_t max_internal)
    {
      for (unsigned i = 0; i < 10; ++i)
      {
        expect<block_info> next = blocks.get_value<block_info>(value);
        if (!next)
          return next.error();

        out.push_back(std::move(*next));

        MDB_val key{};
        const int err = mdb_cursor_get(&cur, &key, &value, MDB_PREV_DUP);
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          if (out.back().id != block_id(0))
            return {lws::error::bad_blockchain};
          return success();
        }
      }

      const auto add_block = [&cur, &out] (std::uint64_t id) -> expect<void>
      {
        expect<crypto::hash> next = do_get_block_hash(cur, block_id(id));
        if (!next)
          return next.error();
        out.push_back(block_info{block_id(id), std::move(*next)});
        return success();
      };

      const std::uint64_t checkpoint = lws::db::storage::get_checkpoints().get_max_height();
      const std::uint64_t anchor = lmdb::to_native(out.back().id);

      for (unsigned i = 1; i <= max_internal; ++i)
      {
        const std::uint64_t offset = 2 << i;
        if (anchor < offset || anchor - offset < checkpoint)
          break;
        MONERO_CHECK(add_block(anchor - offset));
      }

      if (block_id(checkpoint) < out.back().id)
        MONERO_CHECK(add_block(checkpoint));
      if (out.back().id != block_id(0))
        MONERO_CHECK(add_block(0));

      return success();
    }

    template<typename T>
    expect<T> get_blocks(MDB_cursor& cur, std::size_t max_internal)
    {
      T out{};

      max_internal = std::min(std::size_t(64), max_internal);
      out.reserve(12 + max_internal);

      MDB_val key = lmdb::to_val(blocks_version);
      MDB_val value{};
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_SET));
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_LAST_DUP));
      MONERO_CHECK(get_blocks_tail(out, cur, value, max_internal));
      return out;
    }

    template<typename T>
    expect<T> get_blocks_from_height(MDB_cursor& cur, std::size_t max_internal, block_id last_pow)
    {
      T out{};

      max_internal = std::min(std::size_t(64), max_internal);
      out.reserve(12 + max_internal);

      MDB_val key = lmdb::to_val(blocks_version);
      MDB_val value = lmdb::to_val(last_pow);
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_GET_BOTH));
      MONERO_CHECK(get_blocks_tail(out, cur, value, max_internal));
      return out;
    }

    template<typename T>
    expect<T> get_pow_blocks(MDB_cursor& cur, std::size_t max_internal)
    {
      T out{};

      max_internal = std::min(std::size_t(64), max_internal);
      out.reserve(max_internal);

      MDB_val key = lmdb::to_val(pows_version);
      MDB_val value{};
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_SET));
      MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_LAST_DUP));

      for (unsigned i = 0; i < max_internal; ++i)
      {
        expect<block_pow> next = pows.get_value<block_pow>(value);
        if (!next)
          return next.error();

        out.push_back(std::move(*next));

        MDB_val key{};
        const int err = mdb_cursor_get(&cur, &key, &value, MDB_PREV_DUP);
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          if (out.back().id != block_id(0))
            return {lws::error::bad_blockchain};
          return out;
        }
      }
      return out;
    }

    expect<account_id> find_last_id(MDB_cursor& cur) noexcept
    {
      account_id best = account_id(0);

      MDB_val key{};
      MDB_val value{};

      int err = mdb_cursor_get(&cur, &key, &value, MDB_FIRST);
      if (err == MDB_NOTFOUND)
        return best;
      if (err)
        return {lmdb::error(err)};

      do
      {
        MONERO_LMDB_CHECK(mdb_cursor_get(&cur, &key, &value, MDB_LAST_DUP));
        const expect<account_id> current =
          accounts.get_value<MONERO_FIELD(account, id)>(value);
        if (!current)
          return current.error();


        best = std::max(best, *current);
        err = mdb_cursor_get(&cur, &key, &value, MDB_NEXT_NODUP);
        if (err == MDB_NOTFOUND)
          return best;
      } while (err == 0);
      return {lmdb::error(err)};
    }
  } // anonymous

  struct storage_internal : lmdb::database
  {
    struct tables_
    {
      MDB_dbi blocks;
      MDB_dbi pows;
      MDB_dbi accounts;
      MDB_dbi accounts_ba;
      MDB_dbi accounts_bh;
      MDB_dbi outputs;
      MDB_dbi spends;
      MDB_dbi images;
      MDB_dbi requests;
      MDB_dbi webhooks;
      MDB_dbi events;
      MDB_dbi subaddress_ranges;
      MDB_dbi subaddress_indexes;
    } tables;

    const unsigned create_queue_max;

    explicit storage_internal(lmdb::environment env, unsigned create_queue_max)
      : lmdb::database(std::move(env)), tables{}, create_queue_max(create_queue_max)
    {
      lmdb::write_txn txn = this->create_write_txn().value();
      assert(txn != nullptr);

      tables.blocks      = blocks.open(*txn).value();
      tables.pows        = pows.open(*txn).value();
      tables.accounts    = accounts.open(*txn).value();
      tables.accounts_ba = accounts_by_address.open(*txn).value();
      tables.accounts_bh = accounts_by_height.open(*txn).value();
      tables.outputs     = outputs.open(*txn).value();
      tables.spends      = spends.open(*txn).value();
      tables.images      = images.open(*txn).value();
      tables.requests    = requests.open(*txn).value();
      tables.webhooks    = webhooks.open(*txn).value();
      tables.events      = events_by_account_id.open(*txn).value();
      tables.subaddress_ranges  = subaddress_ranges.open(*txn).value();
      tables.subaddress_indexes = subaddress_indexes.open(*txn).value(); 

      const auto v0_outputs = outputs_v0.open(*txn);
      if (v0_outputs)
        MONERO_UNWRAP(convert_table<v0::output, output>(*txn, *v0_outputs, tables.outputs));
      else if (v0_outputs != lmdb::error(MDB_NOTFOUND))
        MONERO_THROW(v0_outputs.error(), "Error opening old outputs table");

      const auto v1_outputs = outputs_v1.open(*txn);
      if (v1_outputs)
        MONERO_UNWRAP(convert_table<v1::output, output>(*txn, *v1_outputs, tables.outputs));
      else if (v1_outputs != lmdb::error(MDB_NOTFOUND))
        MONERO_THROW(v1_outputs.error(), "Error opening old outputs table");

      const auto v0_spends = spends_v0.open(*txn);
      if (v0_spends)
        MONERO_UNWRAP(convert_table<v0::spend, spend>(*txn, *v0_spends, tables.spends));
      else if (v0_spends != lmdb::error(MDB_NOTFOUND))
        MONERO_THROW(v0_spends.error(), "Error opening old spends table");

      check_blockchain(*txn, tables.blocks);
      check_pow(*txn, tables.pows);
      MONERO_UNWRAP(this->commit(std::move(txn)));
    }
  };

  storage_reader::~storage_reader() noexcept
  {}

  expect<block_info> storage_reader::get_last_block() noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.blocks, curs.blocks_cur));

    MDB_val key = lmdb::to_val(blocks_version);
    MDB_val value{};
    MONERO_LMDB_CHECK(mdb_cursor_get(curs.blocks_cur.get(), &key, &value, MDB_SET));
    MONERO_LMDB_CHECK(mdb_cursor_get(curs.blocks_cur.get(), &key, &value, MDB_LAST_DUP));

    return blocks.get_value<block_info>(value);
  }

  expect<block_pow> storage_reader::get_last_pow_block() noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    cursor::pow pow_cur;
    MONERO_CHECK(check_cursor(*txn, db->tables.pows, pow_cur));

    MDB_val key = lmdb::to_val(pows_version);
    MDB_val value{};
    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_SET));
    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_LAST_DUP));

    return pows.get_value<block_pow>(value);
  }

  expect<crypto::hash> storage_reader::get_block_hash(const block_id height) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    MONERO_CHECK(check_cursor(*txn, db->tables.blocks, curs.blocks_cur));
    assert(curs.blocks_cur != nullptr);

    return do_get_block_hash(*curs.blocks_cur, height);
  }

  expect<std::list<crypto::hash>> storage_reader::get_chain_sync()
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.blocks, curs.blocks_cur));
    auto blocks = get_blocks<std::vector<block_info>>(*curs.blocks_cur, 64);
    if (!blocks)
      return blocks.error();

    std::list<crypto::hash> out{};
    for (block_info const& block : *blocks)
      out.push_back(block.hash);
    return out;
  }

  expect<std::list<crypto::hash>> storage_reader::get_pow_sync()
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    cursor::pow pow_cur;
    MONERO_CHECK(check_cursor(*txn, db->tables.pows, pow_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.blocks, curs.blocks_cur));

    MDB_val key = lmdb::to_val(pows_version);
    MDB_val value{};

    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_SET));
    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_LAST_DUP));

    const block_id pow_height =
      MONERO_UNWRAP(pows.get_value<MONERO_FIELD(block_pow, id)>(value));

    auto blocks = get_blocks_from_height<std::vector<block_info>>(*curs.blocks_cur, 64, pow_height);
    if (!blocks)
      return blocks.error();

    std::list<crypto::hash> out{};
    for (block_info const& block : *blocks)
      out.push_back(block.hash);
    return out;
  }

  expect<pow_window>storage_reader::get_pow_window(const db::block_id last)
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    pow_window out{};
    if (last == block_id(0))
      return out;

    std::uint64_t next = 0;
    static_assert(1 <= DIFFICULTY_BLOCKS_COUNT, "invalid DIFFICULTY_BLOCKS_COUNT value");
    if (block_id(DIFFICULTY_BLOCKS_COUNT) < last)
      next = std::uint64_t(last) - (DIFFICULTY_BLOCKS_COUNT - 1);

    cursor::pow pow_cur;
    MONERO_CHECK(check_cursor(*txn, db->tables.pows, pow_cur));

    MDB_val key = lmdb::to_val(pows_version);
    MDB_val value = lmdb::to_val(next);
    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_GET_BOTH));
    for (;;)
    {
      const auto insert = MONERO_UNWRAP(pows.get_value<block_pow>(value));
      out.pow_timestamps.push_back(insert.timestamp);
      out.cumulative_diffs.push_back(insert.cumulative_diff.get_difficulty());

      ++next;
      if (next == std::uint64_t(last) + 1)
        break;

      MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_NEXT_DUP));
    }

    if (last < db::block_id(BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW))
      return out;

    next = std::uint64_t(last) - (BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW - 1);
    key = lmdb::to_val(pows_version);
    value = lmdb::to_val(next);
    MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_GET_BOTH));
    for (;;)
    {
      out.median_timestamps.push_back(
        MONERO_UNWRAP(pows.get_value<MONERO_FIELD(block_pow, timestamp)>(value))
      );

      ++next;
      if (next == std::uint64_t(last) + 1)
        break;
      MONERO_LMDB_CHECK(mdb_cursor_get(pow_cur.get(), &key, &value, MDB_NEXT_DUP));
    }
    return out;
  }

  expect<lmdb::key_stream<account_status, account, cursor::close_accounts>>
  storage_reader::get_accounts(cursor::accounts cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr); // both are moved in pairs
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts, cur));
    return accounts.get_key_stream(std::move(cur));
  }

  expect<lmdb::value_stream<account, cursor::close_accounts>>
  storage_reader::get_accounts(account_status status, cursor::accounts cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr); // both are moved in pairs
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts, cur));
    return accounts.get_value_stream(status, std::move(cur));
  }

  expect<account> storage_reader::get_account(const account_status status, const account_id id) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    cursor::accounts cur;
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts, cur));
    assert(cur != nullptr);

    MDB_val key = lmdb::to_val(status);
    MDB_val value = lmdb::to_val(id);
    const int err = mdb_cursor_get(cur.get(), &key, &value, MDB_GET_BOTH);
    if (err)
    {
      if (err == MDB_NOTFOUND)
        return {lws::error::account_not_found};
      return {lmdb::error(err)};
    }

    return accounts.get_value<account>(value);
  }

  expect<std::pair<account_status, account>>
  storage_reader::get_account(account_address const& address) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    MONERO_CHECK(check_cursor(*txn, db->tables.accounts_ba, curs.accounts_ba_cur));

    MDB_val key = lmdb::to_val(by_address_version);
    MDB_val value = lmdb::to_val(address);
    const int err = mdb_cursor_get(curs.accounts_ba_cur.get(), &key, &value, MDB_GET_BOTH);
    if (err)
    {
      if (err == MDB_NOTFOUND)
        return {lws::error::account_not_found};
      return {lmdb::error(err)};
    }

    /* Database is only indexing by view public for possible CurveZMQ
       authentication extensions. Verifying both public keys here - the function
       takes the entire address as an argument. */
    static_assert(offsetof(account_by_address, address) == 0, "unexpected field offset");
    if (value.mv_size < sizeof(account_address) || std::memcmp(value.mv_data, &address, sizeof(account_address)) != 0)
      return {lws::error::account_not_found};

    const expect<account_lookup> lookup =
      accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup)>(value);
    if (!lookup)
      return lookup.error();

    const expect<account> user = get_account(lookup->status, lookup->id);
    if (!user)
      return user.error();
    return {{lookup->status, *user}};
  }

  expect<lmdb::value_stream<output, cursor::close_outputs>>
  storage_reader::get_outputs(account_id id, cursor::outputs cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.outputs, cur));
    return outputs.get_value_stream(id, std::move(cur));
  }

  expect<lmdb::value_stream<spend, cursor::close_spends>>
  storage_reader::get_spends(account_id id, cursor::spends cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.spends, cur));
    return spends.get_value_stream(id, std::move(cur));
  }

  expect<lmdb::value_stream<db::key_image, cursor::close_images>>
  storage_reader::get_images(output_id id, cursor::images cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.images, cur));
    return images.get_value_stream(id, std::move(cur));
  }

  expect<lmdb::key_stream<request, request_info, cursor::close_requests>>
  storage_reader::get_requests(cursor::requests cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.requests, cur));
    return requests.get_key_stream(std::move(cur));
  }

  expect<request_info>
  storage_reader::get_request(request type, account_address const& address, cursor::requests cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    MONERO_CHECK(check_cursor(*txn, db->tables.requests, cur));

    MDB_val key = lmdb::to_val(type);
    MDB_val value = lmdb::to_val(address);
    MONERO_LMDB_CHECK(mdb_cursor_get(cur.get(), &key, &value, MDB_GET_BOTH));
    return requests.get_value<request_info>(value);
  }

  expect<std::vector<subaddress_dict>>
  storage_reader::get_subaddresses(account_id id, cursor::subaddress_ranges cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    MONERO_CHECK(check_cursor(*txn, db->tables.subaddress_ranges, cur));

    MDB_val key = lmdb::to_val(id);
    MDB_val value{};
    std::vector<subaddress_dict> ranges{};
    int err = mdb_cursor_get(cur.get(), &key, &value, MDB_SET_KEY);
    if (!err)
    {
      std::size_t count = 0;
      if (mdb_cursor_count(cur.get(), &count) == 0)
        ranges.reserve(count);
    }
    for (;;)
    {
      if (err)
      {
        if (err == MDB_NOTFOUND)
          break;
        return {lmdb::error(err)};
      }
      ranges.push_back(MONERO_UNWRAP(subaddress_ranges.get_value(value)));
      err = mdb_cursor_get(cur.get(), &key, &value, MDB_NEXT_DUP);
    }
    return {std::move(ranges)};
  }

  expect<address_index>
  storage_reader::find_subaddress(account_id id, crypto::public_key const& address, cursor::subaddress_indexes& cur) noexcept
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    MONERO_CHECK(check_cursor(*txn, db->tables.subaddress_indexes, cur));
    MDB_val key = lmdb::to_val(id);
    MDB_val value = lmdb::to_val(address);

    MONERO_LMDB_CHECK(mdb_cursor_get(cur.get(), &key, &value, MDB_GET_BOTH));
    return subaddress_indexes.get_value<MONERO_FIELD(subaddress_map, index)>(value);
  }

  expect<std::vector<webhook_value>>
  storage_reader::find_webhook(webhook_key const& key, crypto::hash8 const& payment_id, cursor::webhooks cur)
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.webhooks, cur));

    webhook_dupsort dup{};

    static_assert(sizeof(dup.payment_id) == sizeof(payment_id), "bad memcpy");
    std::memcpy(std::addressof(dup.payment_id), std::addressof(payment_id), sizeof(payment_id));

    MDB_val lkey = lmdb::to_val(key);
    MDB_val lvalue = lmdb::to_val(dup);

    std::vector<webhook_value> result{};
    int err = mdb_cursor_get(cur.get(), &lkey, &lvalue, MDB_GET_BOTH_RANGE);
    for (;;)
    {
      if (err)
      {
        if (err == MDB_NOTFOUND)
          break;
        return {lmdb::error(err)};
      }

      if (webhooks.get_fixed_value<MONERO_FIELD(webhook_dupsort, payment_id)>(lvalue) != dup.payment_id)
        break;

      result.push_back(MONERO_UNWRAP(webhooks.get_value(lvalue)));
      err = mdb_cursor_get(cur.get(), &lkey, &lvalue, MDB_NEXT_DUP);
    }

    return result;
  }

  expect<std::vector<std::pair<webhook_key, std::vector<webhook_value>>>>
  storage_reader::get_webhooks(cursor::webhooks cur)
  {
    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);
    MONERO_CHECK(check_cursor(*txn, db->tables.webhooks, cur));

    std::vector<std::pair<webhook_key, std::vector<webhook_value>>> out;

    MDB_val key{};
    MDB_val value{};
    int err = mdb_cursor_get(cur.get(), &key, &value, MDB_FIRST);
    for (;/* every key */;)
    {
      if (err)
      {
        if (err == MDB_NOTFOUND)
          return {std::move(out)};
        return {lmdb::error(err)};
      }

      out.emplace_back(MONERO_UNWRAP(webhooks.get_key(key)), std::vector<webhook_value>{});

      for (; /* every dup key */ ;)
      {
        if (err)
        {
          if (err == MDB_NOTFOUND)
            break; // inner duplicate key loop
          return {lmdb::error(err)};
        }
        out.back().second.push_back(MONERO_UNWRAP(webhooks.get_value(value)));
        err = mdb_cursor_get(cur.get(), &key, &value, MDB_NEXT_DUP);
      }
      err = mdb_cursor_get(cur.get(), &key, &value, MDB_NEXT);
    }

    return {std::move(out)};
  }

  namespace
  {
    //! `write_bytes` implementation will forward a third argument for `show_keys`.
    template<typename T>
    struct show_keys_wrapper
    {
      T value;
      bool show_keys;
    };

    //! Filter that will instruct type to `show_keys` (or not).
    struct toggle_key_output
    {
      const bool show_keys;

      template<typename T>
      show_keys_wrapper<T> operator()(T value) const noexcept
      {
        return {std::move(value), show_keys};
      }
    };

    struct output_id_key
    {
      std::string operator()(const output_id id) const
      {
        return std::to_string(id.high) + ":" + std::to_string(id.low);
      }
    };

    template<typename T>
    void write_bytes(wire::json_writer& dest, show_keys_wrapper<T> self)
    {
      lws::db::write_bytes(dest, self.value, self.show_keys);
    }
    void write_bytes(wire::json_writer& dest, const account_lookup self)
    {
      wire::object(dest, WIRE_FIELD_COPY(id), WIRE_FIELD_COPY(status));
    }
  }

  // accounts_by_height is output as a sorted array of objects
  static void write_bytes(wire::json_writer& dest, std::pair<block_id, boost::iterator_range<lmdb::value_iterator<account_lookup>>> self)
  {
    wire::object(dest,
      wire::field("scan_height", self.first),
      wire::field("accounts", wire::array(std::move(self.second)))
    );
  }

  static void write_bytes(wire::json_writer& dest, const std::pair<webhook_key, std::vector<webhook_value>>& self)
  {
    wire::object(dest,
      wire::field("key", std::cref(self.first)),
      wire::field("value", std::cref(self.second))
    );
  }

  static void write_bytes(wire::json_writer& dest, const std::pair<lws::db::account_id, std::vector<db::subaddress_dict>>& self)
  {
    wire::object(dest,
      wire::field("id", std::cref(self.first)),
      wire::field("subaddress_indexes", std::cref(self.second))
    );
  }

  expect<void> storage_reader::json_debug(std::ostream& out, bool show_keys)
  {
    using boost::adaptors::reverse;
    using boost::adaptors::transform;

    MONERO_PRECOND(txn != nullptr);
    assert(db != nullptr);

    const auto address_as_key = [](account_by_address const& src)
    {
      return std::make_pair(address_string(src.address), src.lookup);
    };

    cursor::pow pow_cur;
    cursor::accounts accounts_cur;
    cursor::outputs outputs_cur;
    cursor::spends spends_cur;
    cursor::images images_cur;
    cursor::requests requests_cur;
    cursor::webhooks webhooks_cur;
    cursor::webhooks events_cur;
    cursor::subaddress_ranges ranges_cur;
    cursor::subaddress_indexes indexes_cur;

    MONERO_CHECK(check_cursor(*txn, db->tables.blocks, curs.blocks_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.pows, pow_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts, accounts_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts_ba, curs.accounts_ba_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.accounts_bh, curs.accounts_bh_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.outputs, outputs_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.spends, spends_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.images, images_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.requests, requests_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.webhooks, webhooks_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.events, events_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.subaddress_ranges, ranges_cur));
    MONERO_CHECK(check_cursor(*txn, db->tables.subaddress_indexes, indexes_cur));

    auto blocks_partial =
      get_blocks<boost::container::static_vector<block_info, 12>>(*curs.blocks_cur, 0);
    if (!blocks_partial)
      return blocks_partial.error();

    auto pow_partial =
      get_pow_blocks<boost::container::static_vector<block_pow, 12>>(*pow_cur, 12);
    if (!pow_partial)
      return pow_partial.error();

    auto accounts_stream = accounts.get_key_stream(std::move(accounts_cur));
    if (!accounts_stream)
      return accounts_stream.error();

    auto accounts_ba_stream = accounts_by_address.get_value_stream(
      by_address_version, std::move(curs.accounts_ba_cur)
    );
    if (!accounts_ba_stream)
      return accounts_ba_stream.error();

    auto accounts_bh_stream = accounts_by_height.get_key_stream(
      std::move(curs.accounts_bh_cur)
    );
    if (!accounts_bh_stream)
      return accounts_bh_stream.error();

    auto outputs_stream = outputs.get_key_stream(std::move(outputs_cur));
    if (!outputs_stream)
      return outputs_stream.error();

    auto spends_stream = spends.get_key_stream(std::move(spends_cur));
    if (!spends_stream)
      return spends_stream.error();

    auto images_stream = images.get_key_stream(std::move(images_cur));
    if (!images_stream)
      return images_stream.error();

    auto requests_stream = requests.get_key_stream(std::move(requests_cur));
    if (!requests_stream)
      return requests_stream.error();

    const auto ranges_data = subaddress_ranges.get_all(*ranges_cur);
    if (!ranges_data)
        return ranges_data.error();

    auto indexes_stream = subaddress_indexes.get_key_stream(std::move(indexes_cur));
    if (!indexes_stream)
      return indexes_stream.error();

    // This list should be smaller ... ?
    const auto webhooks_data = webhooks.get_all(*webhooks_cur);
    if (!webhooks_data)
      return webhooks_data.error();

    auto events_stream = events_by_account_id.get_key_stream(std::move(events_cur));
    if (!events_stream)
      return events_stream.error();

    const wire::as_array_filter<toggle_key_output> toggle_keys_filter{{show_keys}};
    wire::json_stream_writer json_stream{out};
    wire::object(json_stream,
      wire::field(blocks.name, wire::array(reverse(*blocks_partial))),
      wire::field(pows.name, wire::array(reverse(*pow_partial))),
      wire::field(accounts.name, wire::as_object(accounts_stream->make_range(), wire::enum_as_string, toggle_keys_filter)),
      wire::field(accounts_by_address.name, wire::as_object(transform(accounts_ba_stream->make_range(), address_as_key))),
      wire::field(accounts_by_height.name, wire::array(accounts_bh_stream->make_range())),
      wire::field(outputs.name, wire::as_object(outputs_stream->make_range(), wire::as_integer, wire::as_array)),
      wire::field(spends.name, wire::as_object(spends_stream->make_range(), wire::as_integer, wire::as_array)),
      wire::field(images.name, wire::as_object(images_stream->make_range(), output_id_key{}, wire::as_array)),
      wire::field(requests.name, wire::as_object(requests_stream->make_range(), wire::enum_as_string, toggle_keys_filter)),
      wire::field(subaddress_ranges.name, std::cref(*ranges_data)),
      wire::field(subaddress_indexes.name, wire::as_object(indexes_stream->make_range(), wire::as_integer, wire::as_array)),
      wire::field(webhooks.name, std::cref(*webhooks_data)),
      wire::field(events_by_account_id.name, wire::as_object(events_stream->make_range(), wire::as_integer, wire::as_array))
    );
    json_stream.finish();

    curs.accounts_ba_cur = accounts_ba_stream->give_cursor();
    curs.accounts_bh_cur = accounts_bh_stream->give_cursor();

    if (!out.good())
      return {std::io_errc::stream};
    return success();
  }

  lmdb::suspended_txn storage_reader::finish_read() noexcept
  {
    if (txn != nullptr)
    {
      assert(db != nullptr);
      auto suspended = db->reset_txn(std::move(txn));
      if (suspended) // errors not currently logged
        return {std::move(*suspended)};
    }
    return nullptr;
  }

  cryptonote::checkpoints const& storage::get_checkpoints()
  {
    struct initializer
    {
      cryptonote::checkpoints data;

      initializer()
        : data()
      {
        data.init_default_checkpoints(lws::config::network);

        std::string const* genesis_tx = nullptr;
        std::uint32_t genesis_nonce = 0;

        switch (lws::config::network)
        {
        case cryptonote::TESTNET:
          genesis_tx = std::addressof(::config::testnet::GENESIS_TX);
          genesis_nonce = ::config::testnet::GENESIS_NONCE;
          break;

        case cryptonote::STAGENET:
          genesis_tx = std::addressof(::config::stagenet::GENESIS_TX);
          genesis_nonce = ::config::stagenet::GENESIS_NONCE;
          break;

        case cryptonote::MAINNET:
          genesis_tx = std::addressof(::config::GENESIS_TX);
          genesis_nonce = ::config::GENESIS_NONCE;
          break;

        default:
          MONERO_THROW(lws::error::bad_blockchain, "Unsupported net type");
        }
        cryptonote::block b;
        cryptonote::generate_genesis_block(b, *genesis_tx, genesis_nonce);
        crypto::hash block_hash = cryptonote::get_block_hash(b);
        if (!data.add_checkpoint(0, epee::to_hex::string(epee::as_byte_span(block_hash))))
          MONERO_THROW(lws::error::bad_blockchain, "Genesis tx and checkpoints file mismatch");
      }
    };
    static const initializer instance;
    return instance.data;
  }

  block_info storage::get_last_checkpoint()
  {
    const auto& checkpoints = get_checkpoints().get_points();
    if (checkpoints.empty())
      MONERO_THROW(error::bad_blockchain, "Checkpoints invalid");

    const auto last = checkpoints.rbegin();
    return block_info{block_id(last->first), last->second};
  } 

  storage storage::open(const char* path, unsigned create_queue_max)
  {
    return {
      std::make_shared<storage_internal>(
        MONERO_UNWRAP(lmdb::open_environment(path, 20)), create_queue_max
      )
    };
  }

  storage::~storage() noexcept
  {}

  storage storage::clone() const noexcept
  {
    return storage{db};
  }

  expect<storage_reader> storage::start_read(lmdb::suspended_txn txn) const
  {
    MONERO_PRECOND(db != nullptr);

    expect<lmdb::read_txn> reader = db->create_read_txn(std::move(txn));
    if (!reader)
      return reader.error();

    assert(*reader != nullptr);
    return storage_reader{db, std::move(*reader)};
  }

  namespace // sub functions for `sync_chain(...)`
  {
    expect<void>
    rollback_spends(account_id user, block_id height, MDB_cursor& spends_cur, MDB_cursor& images_cur) noexcept
    {
      MDB_val key = lmdb::to_val(user);
      MDB_val value = lmdb::to_val(height);

      const int err = mdb_cursor_get(&spends_cur, &key, &value, MDB_GET_BOTH_RANGE);
      if (err == MDB_NOTFOUND)
        return success();
      if (err)
        return {lmdb::error(err)};

      for (;;)
      {
        const expect<output_id> out = spends.get_value<MONERO_FIELD(spend, source)>(value);
        if (!out)
          return out.error();

        const expect<crypto::key_image> image =
          spends.get_value<MONERO_FIELD(spend, image)>(value);
        if (!image)
          return image.error();

        key = lmdb::to_val(*out);
        value = lmdb::to_val(*image);
        MONERO_LMDB_CHECK(mdb_cursor_get(&images_cur, &key, &value, MDB_GET_BOTH));
        MONERO_LMDB_CHECK(mdb_cursor_del(&images_cur, 0));

        MONERO_LMDB_CHECK(mdb_cursor_del(&spends_cur, 0));
        const int err = mdb_cursor_get(&spends_cur, &key, &value, MDB_NEXT_DUP);
        if (err == MDB_NOTFOUND)
          break;
        if (err)
          return {lmdb::error(err)};
      }
      return success();
    }

    expect<void>
    rollback_outputs(account_id user, block_id height, MDB_cursor& outputs_cur) noexcept
    {
      MDB_val key = lmdb::to_val(user);
      MDB_val value = lmdb::to_val(height);

      const int err = mdb_cursor_get(&outputs_cur, &key, &value, MDB_GET_BOTH_RANGE);
      if (err == MDB_NOTFOUND)
        return success();
      if (err)
        return {lmdb::error(err)};

      for (;;)
      {
        MONERO_LMDB_CHECK(mdb_cursor_del(&outputs_cur, 0));
        const int err = mdb_cursor_get(&outputs_cur, &key, &value, MDB_NEXT_DUP);
        if (err == MDB_NOTFOUND)
          break;
        if (err)
          return {lmdb::error(err)};
      }
      return success();
    }

    expect<void> rollback_accounts(storage_internal::tables_ const& tables, MDB_txn& txn, block_id height)
    {
      cursor::accounts_by_height accounts_bh_cur;
      MONERO_CHECK(check_cursor(txn, tables.accounts_bh, accounts_bh_cur));

      MDB_val key = lmdb::to_val(height);
      MDB_val value{};
      const int err = mdb_cursor_get(accounts_bh_cur.get(), &key, &value, MDB_SET_RANGE);
      if (err == MDB_NOTFOUND)
        return success();
      if (err)
        return {lmdb::error(err)};

      std::vector<account_lookup> new_by_heights{};

      cursor::accounts accounts_cur;
      cursor::outputs outputs_cur;
      cursor::spends spends_cur;
      cursor::images images_cur;

      MONERO_CHECK(check_cursor(txn, tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, tables.outputs, outputs_cur));
      MONERO_CHECK(check_cursor(txn, tables.spends, spends_cur));
      MONERO_CHECK(check_cursor(txn, tables.images, images_cur));

      const std::uint64_t new_height = std::uint64_t(std::max(height, block_id(1))) - 1;

      // rollback accounts
      for (;;)
      {
        const expect<account_lookup> lookup =
          accounts_by_height.get_value<account_lookup>(value);
        if (!lookup)
          return lookup.error();

        key = lmdb::to_val(lookup->status);
        value = lmdb::to_val(lookup->id);

        MONERO_LMDB_CHECK(mdb_cursor_get(accounts_cur.get(), &key, &value, MDB_GET_BOTH));
        expect<account> user = accounts.get_value<account>(value);
        if (!user)
          return user.error();

        user->scan_height = block_id(new_height);
        user->start_height = std::min(user->scan_height, user->start_height);

        value = lmdb::to_val(*user);
        MONERO_LMDB_CHECK(mdb_cursor_put(accounts_cur.get(), &key, &value, MDB_CURRENT));

        new_by_heights.push_back(account_lookup{user->id, lookup->status});
        MONERO_CHECK(rollback_outputs(user->id, height, *outputs_cur));
        MONERO_CHECK(rollback_spends(user->id, height, *spends_cur, *images_cur));

        MONERO_LMDB_CHECK(mdb_cursor_del(accounts_bh_cur.get(), 0));
        int err = mdb_cursor_get(accounts_bh_cur.get(), &key, &value, MDB_NEXT_DUP);
        if (err == MDB_NOTFOUND)
        {
          err = mdb_cursor_get(accounts_bh_cur.get(), &key, &value, MDB_NEXT_NODUP);
          if (err == MDB_NOTFOUND)
            break;
        }
        if (err)
          return {lmdb::error(err)};
      }

      return bulk_insert(*accounts_bh_cur, new_height, epee::to_span(new_by_heights));
    }

    expect<void> rollback_events(storage_internal::tables_ const& tables, MDB_txn& txn, const block_id height)
    {
      cursor::webhooks webhooks_cur;
      cursor::events   events_cur;
      MONERO_CHECK(check_cursor(txn, tables.webhooks, webhooks_cur));
      MONERO_CHECK(check_cursor(txn, tables.events, events_cur));

      MDB_val key = lmdb::to_val(height);
      MDB_val value{};

      int err = mdb_cursor_get(events_cur.get(), &key, &value, MDB_LAST);
      for ( ; /* every user */ ; )
      {
        for ( ; /* every event */ ;)
        {
          if (err)
          {
            if (err == MDB_NOTFOUND)
              return success();
            return {lmdb::error(err)};
          }

          const webhook_event event =
            MONERO_UNWRAP(events_by_account_id.get_value<webhook_event>(value));

          if (event.link.tx.height < height)
            break; // inner for loop

          MONERO_LMDB_CHECK(mdb_cursor_del(events_cur.get(), 0));
          err = mdb_cursor_get(events_cur.get(), &key, &value, MDB_PREV);
        }
        err = mdb_cursor_get(events_cur.get(), &key, &value, MDB_PREV_NODUP);
      }
      return success();
    }

    expect<void> rollback_chain(storage_internal::tables_ const& tables, MDB_txn& txn, MDB_cursor& cur, block_id height)
    {
      MDB_val key;
      MDB_val value;

      // rollback chain
      int err = 0;
      do
      {
        MONERO_LMDB_CHECK(mdb_cursor_del(&cur, 0));
        err = mdb_cursor_get(&cur, &key, &value, MDB_NEXT_DUP);
      } while (err == 0);

      // rollback pow
      {
        cursor::pow pow_cur;
        MONERO_CHECK(check_cursor(txn, tables.pows, pow_cur));
        
        MDB_val key = lmdb::to_val(pows_version);
        MDB_val value = lmdb::to_val(height);
        int err = mdb_cursor_get(pow_cur.get(), &key, &value, MDB_GET_BOTH);
        for (;;)
        {
          if (err)
          {
            if (err == MDB_NOTFOUND)
              break;
            return {lmdb::error(err)};
          }
          MONERO_LMDB_CHECK(mdb_cursor_del(pow_cur.get(), 0));
          err = mdb_cursor_get(pow_cur.get(), &key, &value, MDB_NEXT_DUP);
        }
      }

      if (err != MDB_NOTFOUND)
        return {lmdb::error(err)};

      MONERO_CHECK(rollback_accounts(tables, txn, height));
      return rollback_events(tables, txn, height);
    }

    template<typename T>
    expect<void> append_block_hashes(MDB_cursor& cur, db::block_id first, T const& chain)
    {
      std::uint64_t height = std::uint64_t(first);
      boost::container::static_vector<block_info, 25> hashes{};
      static_assert(sizeof(hashes) <= 1024, "using more stack space than expected");

      for (auto current = chain.begin() ;; ++current)
      {
        if (current == chain.end() || hashes.size() == hashes.capacity())
        {
          // always overwrite, for pow case (where pows is catching up to blocks)
          MONERO_CHECK(bulk_insert(cur, blocks_version, epee::to_span(hashes), 0));
          if (current == chain.end())
            return success();
          hashes.clear();
        }

        hashes.push_back(block_info{db::block_id(height), *current});
        ++height;
      }
    }

    template<typename T>
    expect<void> append_pow(MDB_cursor& cur, db::block_id first, T const& chain)
    {
      std::uint64_t height = std::uint64_t(first);
      boost::container::static_vector<block_pow, 31> pows{};
      static_assert(sizeof(pows) <= 1024, "using more stack space than expected");

      for (auto current = chain.begin() ;; ++current)
      {
        if (current == chain.end() || pows.size() == pows.capacity())
        {
          MONERO_CHECK(bulk_insert(cur, pows_version, epee::to_span(pows)));
          if (current == chain.end())
            return success();
          pows.clear();
        }

        pows.push_back(block_pow{db::block_id(height), current->timestamp, current->cumulative_diff});
        ++height;
      }
    }

  } // anonymous

  expect<void> storage::rollback(block_id height)
  {
    MONERO_PRECOND(db != nullptr);

    return db->try_write([this, height] (MDB_txn& txn) -> expect<void>
    {
      cursor::blocks blocks_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));

      MDB_val key = lmdb::to_val(blocks_version);
      MDB_val value = lmdb::to_val(height);
      const int err = mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_GET_BOTH);
      if (err == MDB_NOTFOUND)
        return success();
      if (err)
        return {lmdb::error(err)};

      return rollback_chain(this->db->tables, txn, *blocks_cur, height);
    });
  }

  expect<void> storage::sync_chain(block_id height, epee::span<const crypto::hash> hashes)
  {
    MONERO_PRECOND(!hashes.empty());
    MONERO_PRECOND(db != nullptr);

    return db->try_write([this, height, hashes] (MDB_txn& txn) -> expect<void>
    {
      cursor::blocks blocks_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));

      expect<crypto::hash> hash = do_get_block_hash(*blocks_cur, height);
      if (!hash)
        return hash.error();

      // the first entry should always match on in the DB
      if (*hash != *(hashes.begin()))
        return {lws::error::bad_blockchain};

      MDB_val key{};
      MDB_val value{};

      std::uint64_t current = std::uint64_t(height) + 1;
      auto first = hashes.begin();
      auto chain = boost::make_iterator_range(++first, hashes.end());
      for ( ; !chain.empty(); chain.advance_begin(1), ++current)
      {
        const int err = mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_NEXT_DUP);
        if (err == MDB_NOTFOUND)
          break;
        if (err)
          return {lmdb::error(err)};

        hash = blocks.get_value<MONERO_FIELD(block_info, hash)>(value);
        if (!hash)
          return hash.error();

        if (*hash != chain.front())
        {
          if (current <= get_checkpoints().get_max_height())
          {
            /* Either the daemon is performing an attack with a fake chain, or
              the daemon is still syncing. */
            MERROR("Attempting rollback past last checkpoint. Wait until daemon finishes syncing - otherwise daemon is performing an attack.");
            return {lws::error::bad_blockchain};
          }

          MONERO_CHECK(rollback_chain(this->db->tables, txn, *blocks_cur, db::block_id(current)));
          break;
        }
      }
      return append_block_hashes(*blocks_cur, db::block_id(current), chain);
    });
  }

  expect<void> storage::sync_pow(block_id height, epee::span<const crypto::hash> hashes, epee::span<const pow_sync> pow)
  {
    MONERO_PRECOND(!hashes.empty());
    MONERO_PRECOND(hashes.size() == pow.size());
    MONERO_PRECOND(db != nullptr);

    return db->try_write([this, height, hashes, pow] (MDB_txn& txn) -> expect<void>
    {
      cursor::blocks blocks_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));
      
      expect<crypto::hash> hash = do_get_block_hash(*blocks_cur, height);
      if (!hash)
        return hash.error();

      // the first entry should always match on in the DB
      if (*hash != *(hashes.begin()))
        return {lws::error::bad_blockchain};

      MDB_val key{};
      MDB_val value{};

      std::uint64_t current = std::uint64_t(height) + 1;
      auto first = hashes.begin();
      auto chain = boost::make_iterator_range(++first, hashes.end());
      const auto& checkpoints = get_checkpoints();
      for ( ; !chain.empty(); chain.advance_begin(1), ++current)
      {
        // if while syncing from beginning, a checkpoint was missed
        const auto checkpoint = checkpoints.get_points().find(current);
        if (checkpoint != checkpoints.get_points().end() && checkpoint->second != chain.front())
        {
          MERROR("Missed a checkpoint during sync_pow");
          return {error::bad_blockchain};
        }

        const int err = mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_NEXT_DUP);
        if (err == MDB_NOTFOUND)
          break;
        if (err)
          return {lmdb::error(err)};

        auto full_value = blocks.get_value<block_info>(value);
        if (!full_value)
          return full_value.error();
        if (full_value->id != block_id(current)) // hit a checkpoint or other block that is ahead of pow
          break;

        if (full_value->hash != chain.front())
        {
          if (current <= checkpoints.get_max_height())
          {
            MERROR("Attempting rollback past last checkpoint; invalid daemon chain response");
            return {lws::error::bad_blockchain};
          }
          MONERO_CHECK(rollback_chain(this->db->tables, txn, *blocks_cur, db::block_id(current)));
          break;
        }
      }

      // scan checkpoints, this is hardened mode!
      {
        std::uint64_t current_copy = current;
        for (const auto& current_hash : chain)
        {
          // if while syncing from beginning, a checkpoint was missed
          const auto checkpoint = checkpoints.get_points().find(current_copy);
          if (checkpoint != checkpoints.get_points().end() && checkpoint->second != current_hash)
          {
            MERROR("Missed a checkpoint during sync_pow");
            return {error::bad_blockchain};
          }
          ++current_copy;
        }
      }

      auto first_pow = pow.begin() + std::ptrdiff_t(chain.begin() - hashes.begin());

      cursor::pow pow_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.pows, pow_cur));
      MONERO_CHECK(append_block_hashes(*blocks_cur, db::block_id(current), chain));
      return append_pow(*pow_cur, db::block_id(current), boost::make_iterator_range(first_pow, pow.end()));
   });
  }

  namespace
  {
    expect<db::account_time> get_account_time() noexcept
    {
      const auto time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      );

      if (time.count() < 0)
        return {lws::error::system_clock_invalid_range};
      if (std::numeric_limits<lmdb::native_type<db::account_time>>::max() < time.count())
        return {lws::error::system_clock_invalid_range};
      return db::account_time(time.count());
    }
  }

  expect<void> storage::update_access_time(account_address const& address) noexcept
  {
    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, &address] (MDB_txn& txn) -> expect<void>
    {
      const expect<db::account_time> current_time = get_account_time();
      if (!current_time)
        return current_time.error();

      cursor::accounts accounts_cur;
      cursor::accounts accounts_ba_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));

      MDB_val key = lmdb::to_val(by_address_version);
      MDB_val value = lmdb::to_val(address);
      const int err = mdb_cursor_get(accounts_ba_cur.get(), &key, &value, MDB_GET_BOTH);

      if (err == MDB_NOTFOUND)
        return {lws::error::account_not_found};
      if (err)
        return {lmdb::error(err)};

      const expect<account_lookup> lookup =
        accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup)>(value);
      if (!lookup)
        return lookup.error();

      key = lmdb::to_val(lookup->status);
      value = lmdb::to_val(lookup->id);
      MONERO_LMDB_CHECK(mdb_cursor_get(accounts_cur.get(), &key, &value, MDB_GET_BOTH));

      expect<account> user = accounts.get_value<account>(value);
      if (!user)
        return user.error();

      user->access = *current_time;
      value = lmdb::to_val(*user);
      MONERO_LMDB_CHECK(mdb_cursor_put(accounts_cur.get(), &key, &value, MDB_CURRENT));
      return success();
    });
  }

  expect<std::vector<account_address>>
  storage::change_status(account_status status , epee::span<const account_address> addresses)
  {
    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, status, addresses] (MDB_txn& txn) -> expect<std::vector<account_address>>
    {
      std::vector<account_address> changed{};
      changed.reserve(addresses.size());

      cursor::accounts accounts_cur;
      cursor::accounts accounts_ba_cur;
      cursor::accounts accounts_bh_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_bh, accounts_bh_cur));

      for (account_address const& address : addresses)
      {
        MDB_val key = lmdb::to_val(by_address_version);
        MDB_val value = lmdb::to_val(address);
        const int err = mdb_cursor_get(accounts_ba_cur.get(), &key, &value, MDB_GET_BOTH);

        if (err == MDB_NOTFOUND)
          continue;
        if (err)
          return {lmdb::error(err)};

        expect<account_by_address> by_address =
          accounts_by_address.get_value<account_by_address>(value);
        if (!by_address)
          return by_address.error();

        const account_status current = by_address->lookup.status;
        if (current != status)
        {
          by_address->lookup.status = status;

          value = lmdb::to_val(*by_address);
          MONERO_LMDB_CHECK(mdb_cursor_put(accounts_ba_cur.get(), &key, &value, MDB_CURRENT));

          key = lmdb::to_val(current);
          value = lmdb::to_val(by_address->lookup.id);
          MONERO_LMDB_CHECK(mdb_cursor_get(accounts_cur.get(), &key, &value, MDB_GET_BOTH));

          expect<account> user = accounts.get_value<account>(value);
          if (!user)
            return user.error();

          MONERO_LMDB_CHECK(mdb_cursor_del(accounts_cur.get(), 0));

          key = lmdb::to_val(status);
          value = lmdb::to_val(*user);
          MONERO_LMDB_CHECK(mdb_cursor_put(accounts_cur.get(), &key, &value, MDB_NODUPDATA));

          key = lmdb::to_val(user->scan_height);
          value = lmdb::to_val(user->id);
          MONERO_LMDB_CHECK(mdb_cursor_get(accounts_bh_cur.get(), &key, &value, MDB_GET_BOTH));

          value = lmdb::to_val(by_address->lookup);
          MONERO_LMDB_CHECK(mdb_cursor_put(accounts_bh_cur.get(), &key, &value, MDB_CURRENT));
        }

        changed.push_back(address);
      }

      return changed;
    });
  }

  namespace
  {
    expect<void> do_add_account(MDB_cursor& accounts_cur, MDB_cursor& accounts_ba_cur, MDB_cursor& accounts_bh_cur, account const& user) noexcept
    {
      {
        crypto::secret_key copy{};
        crypto::public_key verify{};
        static_assert(sizeof(copy) == sizeof(user.key), "bad memcpy");
        std::memcpy(
          std::addressof(unwrap(copy)), std::addressof(user.key), sizeof(copy)
        );

        if (!crypto::secret_key_to_public_key(copy, verify))
          return {lws::error::bad_view_key};

        if (verify != user.address.view_public)
          return {lws::error::bad_view_key};
      }

      const account_status status =
        user.flags == account_flags::admin_account ?
          account_status::hidden : account_status::active;
      const account_by_address by_address{user.address, {user.id, status}};

      MDB_val key = lmdb::to_val(by_address_version);
      MDB_val value = lmdb::to_val(by_address);
      const int err = mdb_cursor_put(&accounts_ba_cur, &key, &value, MDB_NODUPDATA);

      if (err == MDB_KEYEXIST)
        return {lws::error::account_exists};
      if (err)
        return {lmdb::error(err)};

      key = lmdb::to_val(user.scan_height);
      value = lmdb::to_val(by_address.lookup);
      MONERO_LMDB_CHECK(
        mdb_cursor_put(&accounts_bh_cur, &key, &value, MDB_NODUPDATA)
      );

      key = lmdb::to_val(by_address.lookup.status);
      value = lmdb::to_val(user);
      MONERO_LMDB_CHECK(
        mdb_cursor_put(&accounts_cur, &key, &value, MDB_NODUPDATA)
      );
      return success();
    }
  } // anonymous

  expect<void> storage::add_account(account_address const& address, crypto::secret_key const& key, const account_flags flags) noexcept
  {
    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, &address, &key, flags] (MDB_txn& txn) -> expect<void>
    {
      const expect<db::account_time> current_time = get_account_time();
      if (!current_time)
        return current_time.error();

      cursor::blocks blocks_cur;
      cursor::accounts accounts_cur;
      cursor::accounts_by_address accounts_ba_cur;
      cursor::accounts_by_height accounts_bh_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_bh, accounts_bh_cur));

      const expect<account_id> last_id = find_last_id(*accounts_cur);
      if (!last_id)
        return last_id.error();

      MDB_val keyv = lmdb::to_val(blocks_version);
      MDB_val value{};

      MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &keyv, &value, MDB_SET));
      MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &keyv, &value, MDB_LAST_DUP));

      const expect<block_id> height =
        blocks.get_value<MONERO_FIELD(block_info, id)>(value);
      if (!height)
        return height.error();

      const account_id next_id = account_id(lmdb::to_native(*last_id) + 1);
      account user{};
      user.id = next_id;
      user.address = address;
      static_assert(sizeof(user.key) == sizeof(key), "bad memcpy");
      std::memcpy(std::addressof(user.key), std::addressof(key), sizeof(key));
      user.start_height = *height;
      user.scan_height = *height;
      user.access = *current_time;
      user.creation = *current_time;
      user.flags = flags;

      return do_add_account(
        *accounts_cur, *accounts_ba_cur, *accounts_bh_cur, user
      );
    });
  }

  namespace
  {
    //! \return Success, even if `address` was not found (designed for
    expect<void>
    change_height(MDB_cursor& accounts_cur, MDB_cursor& accounts_ba_cur, MDB_cursor& accounts_bh_cur, block_id height, account_address const& address)
    {
      MDB_val key = lmdb::to_val(by_address_version);
      MDB_val value = lmdb::to_val(address);
      const int err = mdb_cursor_get(&accounts_ba_cur, &key, &value, MDB_GET_BOTH);
      if (err == MDB_NOTFOUND)
        return {lws::error::account_not_found};
      if (err)
        return {lmdb::error(err)};

      const expect<account_lookup> lookup =
        accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup)>(value);
      if (!lookup)
        return lookup.error();

      key = lmdb::to_val(lookup->status);
      value = lmdb::to_val(lookup->id);
      MONERO_LMDB_CHECK(
        mdb_cursor_get(&accounts_cur, &key, &value, MDB_GET_BOTH)
      );

      expect<account> user = accounts.get_value<account>(value);
      if (!user)
        return user.error();

      const block_id current_height = user->scan_height;
      user->scan_height = std::min(height, user->scan_height);
      user->start_height = std::min(height, user->start_height);

      value = lmdb::to_val(*user);
      MONERO_LMDB_CHECK(
        mdb_cursor_put(&accounts_cur, &key, &value, MDB_CURRENT)
      );

      key = lmdb::to_val(current_height);
      MONERO_LMDB_CHECK(
        mdb_cursor_get(&accounts_bh_cur, &key, &value, MDB_GET_BOTH)
      );
      MONERO_LMDB_CHECK(mdb_cursor_del(&accounts_bh_cur, 0));

      key = lmdb::to_val(height);
      value = lmdb::to_val(*lookup);
      MONERO_LMDB_CHECK(
        mdb_cursor_put(&accounts_bh_cur, &key, &value, MDB_NODUPDATA)
      );

      return success();
    }
  }

  expect<std::vector<account_address>>
  storage::rescan(db::block_id height, epee::span<const account_address> addresses)
  {
    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, height, addresses] (MDB_txn& txn) -> expect<std::vector<account_address>>
    {
      {
        cursor::blocks blocks_cur;
        MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));

        MDB_val key = lmdb::to_val(blocks_version);
        MDB_val value{};

        MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_SET));
        MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_LAST_DUP));

        const expect<block_id> current_height =
          blocks.get_value<MONERO_FIELD(block_info, id)>(value);
        if (!current_height)
          return current_height.error();
        if (*current_height < height)
          return {error::bad_height};
      }

      std::vector<account_address> updated{};
      updated.reserve(addresses.size());

      cursor::accounts accounts_cur;
      cursor::accounts_by_address accounts_ba_cur;
      cursor::accounts_by_height accounts_bh_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_bh, accounts_bh_cur));

      for (account_address const& address : addresses)
      {
        const expect<void> changed = change_height(
          *accounts_cur, *accounts_ba_cur, *accounts_bh_cur, height, address
        );
        if (changed)
          updated.push_back(address);
        else if (changed != lws::error::account_not_found)
          return changed.error();
      }
      return updated;
    });
  }

  expect<std::vector<webhook_new_account>> storage::creation_request(account_address const& address, crypto::secret_key const& key, account_flags flags) noexcept
  {
    MONERO_PRECOND(db != nullptr);

    if (!db->create_queue_max)
      return {lws::error::create_queue_max};

    return db->try_write([this, &address, &key, flags] (MDB_txn& txn) -> expect<std::vector<webhook_new_account>>
    {
      const expect<db::account_time> current_time = get_account_time();
      if (!current_time)
        return current_time.error();

      cursor::accounts_by_address accounts_ba_cur;
      cursor::blocks blocks_cur;
      cursor::accounts requests_cur;
      cursor::webhooks webhooks_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.requests, requests_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.webhooks, webhooks_cur));

      MDB_val keyv = lmdb::to_val(by_address_version);
      MDB_val value = lmdb::to_val(address);

      int err = mdb_cursor_get(accounts_ba_cur.get(), &keyv, &value, MDB_GET_BOTH);
      if (err != MDB_NOTFOUND)
      {
        if (err)
          return {lmdb::error(err)};
        return {lws::error::account_exists};
      }

      const request req = request::create;
      keyv = lmdb::to_val(req);
      value = MDB_val{};
      err = mdb_cursor_get(requests_cur.get(), &keyv, &value, MDB_SET);
      if (!err)
      {
        mdb_size_t count = 0;
        MONERO_LMDB_CHECK(mdb_cursor_count(requests_cur.get(), &count));
        if (this->db->create_queue_max <= count)
          return {lws::error::create_queue_max};
      }
      else if (err != MDB_NOTFOUND)
        return {lmdb::error(err)};

      keyv = lmdb::to_val(blocks_version);
      value = MDB_val{};

      MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &keyv, &value, MDB_SET));
      MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &keyv, &value, MDB_LAST_DUP));

      const expect<block_id> height =
        blocks.get_value<MONERO_FIELD(block_info, id)>(value);
      if (!height)
        return height.error();

      request_info info{};
      info.address = address;
      static_assert(sizeof(info.key) == sizeof(key), "bad memcpy");
      std::memcpy(std::addressof(info.key), std::addressof(key), sizeof(key));
      info.creation = *current_time;
      info.start_height = *height;
      info.creation_flags = flags;

      keyv = lmdb::to_val(req);
      value = lmdb::to_val(info);

      err = mdb_cursor_put(requests_cur.get(), &keyv, &value, MDB_NODUPDATA);
      if (err == MDB_KEYEXIST)
        return {lws::error::duplicate_request};
      if (err)
        return {lmdb::error(err)};

      std::vector<webhook_new_account> hooks{};
      webhook_key wkey{account_id::invalid, webhook_type::new_account};
      keyv = lmdb::to_val(wkey);
      err = mdb_cursor_get(webhooks_cur.get(), &keyv, &value, MDB_SET_KEY);
      for (;;)
      {
        if (err)
        {
          if (err == MDB_NOTFOUND)
            break;
          return {lmdb::error(err)};
        }

        hooks.push_back(webhook_new_account{MONERO_UNWRAP(webhooks.get_value(value)), address});
        err = mdb_cursor_get(webhooks_cur.get(), &keyv, &value, MDB_NEXT_DUP);
      }

      return hooks;
    });
  }

  expect<void> storage::import_request(account_address const& address, block_id height) noexcept
  {
    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, &address, height] (MDB_txn& txn) -> expect<void>
    {
      const expect<db::account_time> current_time = get_account_time();
      if (!current_time)
        return current_time.error();

      cursor::blocks accounts_ba_cur;
      cursor::requests requests_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.requests, requests_cur));

      MDB_val key = lmdb::to_val(by_address_version);
      MDB_val value = lmdb::to_val(address);

      int err = mdb_cursor_get(accounts_ba_cur.get(), &key, &value, MDB_GET_BOTH);
      if (err == MDB_NOTFOUND)
        return {lws::error::account_not_found};
      if (err)
        return {lmdb::error(err)};

      request_info info{};
      info.address = address;
      info.start_height = height;

      const request req = request::import_scan;
      key = lmdb::to_val(req);
      value = lmdb::to_val(info);

      err = mdb_cursor_put(requests_cur.get(), &key, &value, MDB_NODUPDATA);
      if (err == MDB_KEYEXIST)
        return {lws::error::duplicate_request};
      if (err)
        return {lmdb::error(err)};

      return success();
    });
  }

  namespace
  {
    expect<std::vector<account_address>>
    create_accounts(MDB_txn& txn, storage_internal::tables_ const& tables, epee::span<const account_address> addresses)
    {
      std::vector<account_address> stored{};
      stored.reserve(addresses.size());

      const expect<db::account_time> current_time = get_account_time();
      if (!current_time)
        return current_time.error();

      cursor::accounts accounts_cur;
      cursor::accounts_by_address accounts_ba_cur;
      cursor::accounts_by_height accounts_bh_cur;
      cursor::requests requests_cur;

      MONERO_CHECK(check_cursor(txn, tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, tables.accounts_bh, accounts_bh_cur));
      MONERO_CHECK(check_cursor(txn, tables.requests, requests_cur));

      expect<account_id> last_id = find_last_id(*accounts_cur);
      if (!last_id)
        return last_id.error();

      const request req = request::create;
      for (account_address const& address : addresses)
      {
        MDB_val keyv = lmdb::to_val(req);
        MDB_val value = lmdb::to_val(address);
        int err = mdb_cursor_get(requests_cur.get(), &keyv, &value, MDB_GET_BOTH);
        if (err == MDB_NOTFOUND)
          continue;
        if (err)
          return {lmdb::error(err)};

        const expect<db::request_info> info = requests.get_value<db::request_info>(value);
        if (!info)
          return info.error();

        MONERO_LMDB_CHECK(mdb_cursor_del(requests_cur.get(), 0));

        const account_id next_id = account_id(lmdb::to_native(*last_id) + 1);
        if (next_id == account_id::invalid)
          return {lws::error::account_max};

        account user{};
        user.id = next_id;
        user.address = address;
        user.key = info->key;
        user.start_height = info->start_height;
        user.scan_height = info->start_height;
        user.access = *current_time;
        user.creation = info->creation;
        user.flags = info->creation_flags;

        const expect<void> added =
          do_add_account(*accounts_cur, *accounts_ba_cur, *accounts_bh_cur, user);

        if (!added)
        {
          if (added == lws::error::account_exists || added == lws::error::bad_view_key)
            continue;
          return added.error();
        }

        *last_id = next_id;
        stored.push_back(address);
      }
      return stored;
    }

    expect<std::vector<account_address>>
    import_accounts(MDB_txn& txn, storage_internal::tables_ const& tables, epee::span<const account_address> addresses)
    {
      std::vector<account_address> updated{};
      updated.reserve(addresses.size());

      cursor::accounts accounts_cur;
      cursor::accounts accounts_ba_cur;
      cursor::accounts accounts_bh_cur;
      cursor::requests requests_cur;

      MONERO_CHECK(check_cursor(txn, tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, tables.accounts_bh, accounts_bh_cur));
      MONERO_CHECK(check_cursor(txn, tables.requests, requests_cur));

      const request req = request::import_scan;
      for (account_address const& address : addresses)
      {
        MDB_val key = lmdb::to_val(req);
        MDB_val value = lmdb::to_val(address);
        const int err = mdb_cursor_get(requests_cur.get(), &key, &value, MDB_GET_BOTH);
        if (err == MDB_NOTFOUND)
          continue;
        if (err)
          return {lmdb::error(err)};

        const expect<block_id> new_height =
          requests.get_value<MONERO_FIELD(request_info, start_height)>(value);
        MONERO_LMDB_CHECK(mdb_cursor_del(requests_cur.get(), 0));
        if (!new_height)
          return new_height.error();

        const expect<void> changed = change_height(
          *accounts_cur, *accounts_ba_cur, *accounts_bh_cur, *new_height, address
        );
        if (changed)
          updated.push_back(address);
        else if (changed != lws::error::account_not_found)
          return changed.error();
      }
      return updated;
    }
  } // anonymous

  expect<std::vector<account_address>>
  storage::accept_requests(request req, epee::span<const account_address> addresses)
  {
    if (addresses.empty())
      return std::vector<account_address>{};

    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, req, addresses] (MDB_txn& txn) -> expect<std::vector<account_address>>
    {
      switch (req)
      {
      case request::create:
        return create_accounts(txn, this->db->tables, addresses);
      case request::import_scan:
        return import_accounts(txn, this->db->tables, addresses);
      default:
        break;
      }
      return {common_error::kInvalidArgument};
    });
  }

  expect<std::vector<account_address>>
  storage::reject_requests(request req, epee::span<const account_address> addresses)
  {
    if (addresses.empty())
      return std::vector<account_address>{};

    MONERO_PRECOND(db != nullptr);
    return db->try_write([this, req, addresses] (MDB_txn& txn) -> expect<std::vector<account_address>>
    {
      std::vector<account_address> rejected{};

      cursor::requests requests_cur;
      MONERO_CHECK(check_cursor(txn, this->db->tables.requests, requests_cur));

      MDB_val key = lmdb::to_val(req);
      for (account_address const& address : addresses)
      {
        MDB_val value = lmdb::to_val(address);
        const int err = mdb_cursor_get(requests_cur.get(), &key, &value, MDB_GET_BOTH);
        if (err && err != MDB_NOTFOUND)
          return {lmdb::error(err)};

        if (!err)
        {
          MONERO_LMDB_CHECK(mdb_cursor_del(requests_cur.get(), 0));
          rejected.push_back(address);
        }
      }

      return rejected;
    });
  }

  namespace
  {
    expect<void>
    add_spends(MDB_cursor& spends_cur, MDB_cursor& images_cur, account_id user, epee::span<const spend> spends) noexcept
    {
      MONERO_CHECK(bulk_insert(spends_cur, user, spends));
      for (auto const& entry : spends)
      {
        const db::key_image image{entry.image, entry.link};

        MDB_val key = lmdb::to_val(entry.source);
        MDB_val value = lmdb::to_val(image);
        const int err = mdb_cursor_put(&images_cur, &key, &value, MDB_NODUPDATA);
        if (err && err != MDB_KEYEXIST)
          return {lmdb::error(err)};
      }
      return success();
    }

    expect<void> check_hooks(MDB_cursor& webhooks_cur, MDB_cursor& events_cur, const lws::account& user)
    {
      const account_id user_id = user.id();
      const webhook_key hook_key{user_id, webhook_type::tx_confirmation};

      // check payment_id == x (match specific) webhooks second
      for (const output& out : user.outputs())
      {
        webhook_dupsort sorter{};
        static_assert(sizeof(sorter.payment_id) == sizeof(out.payment_id.short_), "bad memcpy");
        std::memcpy(
          std::addressof(sorter.payment_id), std::addressof(out.payment_id.short_), sizeof(sorter.payment_id)
        );

        MDB_val key = lmdb::to_val(hook_key);
        MDB_val value = lmdb::to_val(sorter);
        int err = mdb_cursor_get(&webhooks_cur, &key, &value, MDB_GET_BOTH_RANGE);

        for (; /* all user/payment_id==x entries */ ;)
        {
          if (err)
          {
            if (err != MDB_NOTFOUND)
              return {lmdb::error(err)};
            break;
          }
          const webhook_dupsort db_sorter = MONERO_UNWRAP(webhooks.get_fixed_value<webhook_dupsort>(value));
          if (db_sorter.payment_id != sorter.payment_id)
            break;

          const webhook_event event{
            webhook_output{out.link, out.spend_meta.id}, db_sorter
          };

          MDB_val ekey = lmdb::to_val(user_id);
          MDB_val evalue = lmdb::to_val(event);
          MONERO_LMDB_CHECK(mdb_cursor_put(&events_cur, &ekey, &evalue, 0));
          err = mdb_cursor_get(&webhooks_cur, &key, &value, MDB_NEXT_DUP);
        }
      }
      return success();
    }

    expect<void>
    add_ongoing_hooks(std::vector<webhook_tx_confirmation>& events, MDB_cursor& webhooks_cur, MDB_cursor& outputs_cur, MDB_cursor& events_cur, const account_id user, const block_id begin, const block_id end)
    {
      if (begin == end)
        return success();

      const webhook_key hook_key{user, webhook_type::tx_confirmation};
      MDB_val key = lmdb::to_val(user);
      MDB_val value{};

      int err = mdb_cursor_get(&events_cur, &key, &value, MDB_SET_KEY);
      for ( ; /* every ongoing event from this user */ ; )
      {
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          return success();
        }

        const webhook_event event =
          MONERO_UNWRAP(events_by_account_id.get_value<webhook_event>(value));

        MDB_val rkey = lmdb::to_val(hook_key);
        MDB_val rvalue = lmdb::to_val(event.link_webhook);
        MONERO_LMDB_CHECK(mdb_cursor_get(&webhooks_cur, &rkey, &rvalue, MDB_GET_BOTH));

        MDB_val okey = lmdb::to_val(user);
        MDB_val ovalue = lmdb::to_val(event.link);
        MONERO_LMDB_CHECK(mdb_cursor_get(&outputs_cur, &okey, &ovalue, MDB_GET_BOTH));

        events.push_back(
          webhook_tx_confirmation{
            MONERO_UNWRAP(webhooks.get_key(rkey)),
            MONERO_UNWRAP(webhooks.get_value(rvalue)),
            MONERO_UNWRAP(outputs.get_value<output>(ovalue))
          }
        );

        const std::uint32_t requested_confirmations =
          events.back().value.second.confirmations;

        events.back().value.second.confirmations =
          lmdb::to_native(begin) - lmdb::to_native(event.link.tx.height) + 1;

        // copy next blocks from first
        for (const auto block_num : boost::counting_range(lmdb::to_native(begin) + 1, lmdb::to_native(end)))
        {
          if (requested_confirmations <= events.back().value.second.confirmations)
            break;
          events.push_back(events.back());
          ++(events.back().value.second.confirmations);
	      }
        if (requested_confirmations <= events.back().value.second.confirmations)
          MONERO_LMDB_CHECK(mdb_cursor_del(&events_cur, 0));
        err = mdb_cursor_get(&events_cur, &key, &value, MDB_NEXT_DUP);
      }
      return success();
    }
  } // anonymous

  expect<std::pair<std::size_t, std::vector<webhook_tx_confirmation>>> storage::update(block_id height, epee::span<const crypto::hash> chain, epee::span<const lws::account> users, epee::span<const pow_sync> pow)
  {
    if (users.empty() && chain.empty())
      return {std::make_pair(0, std::vector<webhook_tx_confirmation>{})};
    MONERO_PRECOND(!chain.empty());
    MONERO_PRECOND(db != nullptr);
    if (!pow.empty())
      MONERO_PRECOND(chain.size() == pow.size());

    return db->try_write([this, height, chain, users, pow] (MDB_txn& txn) -> expect<std::pair<std::size_t, std::vector<webhook_tx_confirmation>>>
    {
      epee::span<const crypto::hash> chain_copy{chain};
      epee::span<const pow_sync> pow_copy{pow};
      const std::uint64_t last_update =
        lmdb::to_native(height) + chain.size() - 1;
      const std::uint64_t first_new = lmdb::to_native(height) + 1;

      // collect all .value() errors
      std::pair<std::size_t, std::vector<webhook_tx_confirmation>> updated;
      if (get_checkpoints().get_max_height() <= last_update)
      {
        cursor::blocks blocks_cur;
        cursor::pow    pow_cur;
        MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));
        MONERO_CHECK(check_cursor(txn, this->db->tables.pows, pow_cur));

        MDB_val key = lmdb::to_val(blocks_version);
        MDB_val value;
        MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_SET));
        MONERO_LMDB_CHECK(mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_LAST_DUP));

        const block_info last_block = MONERO_UNWRAP(blocks.get_value<block_info>(value));
        if (last_block.id < height)
          return {lws::error::bad_blockchain};

        const std::uint64_t last_same =
          std::min(lmdb::to_native(last_block.id), last_update);

        const std::uint64_t offset = last_same - lmdb::to_native(height);
        if (MONERO_UNWRAP(do_get_block_hash(*blocks_cur, block_id(last_same))) != *(chain_copy.begin() + offset))
          return {lws::error::blockchain_reorg};

        chain_copy.remove_prefix(offset + 1);
        MONERO_CHECK(
          append_block_hashes(
            *blocks_cur, block_id(lmdb::to_native(height) + offset + 1), chain_copy
          )
        );
 
        if (!pow_copy.empty())
        {
          pow_copy.remove_prefix(offset + 1);
          MONERO_CHECK(
            append_pow(*pow_cur, block_id(lmdb::to_native(height) + offset + 1), pow_copy)
          );
        }
      }
      else // perform chain/pow hardening via checkpoints (if available)
      {
        cursor::blocks blocks_cur;
        MONERO_CHECK(check_cursor(txn, this->db->tables.blocks, blocks_cur));
 
        MDB_val key = lmdb::to_val(blocks_version);
        MDB_val value = lmdb::to_val(last_update);
        int err = mdb_cursor_get(blocks_cur.get(), &key, &value, MDB_GET_BOTH);

        // verify last block hash if available. If not availble, --untrusted-daemon was not used
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
        }
        else
        {
          const auto cur_block = blocks.get_value<block_info>(value);
          if (!cur_block)
            return cur_block.error();
          // If a reorg past a checkpoint is being attempted            
          if (chain[chain.size() - 1] != cur_block->hash)
            return {error::bad_blockchain};

        }
      }

      cursor::accounts            accounts_cur;
      cursor::accounts_by_address accounts_ba_cur;
      cursor::accounts_by_height  accounts_bh_cur;
      cursor::outputs             outputs_cur;
      cursor::spends              spends_cur;
      cursor::images              images_cur;
      cursor::webhooks            webhooks_cur;
      cursor::events              events_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts, accounts_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_bh, accounts_bh_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.outputs, outputs_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.spends, spends_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.images, images_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.webhooks, webhooks_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.events, events_cur));

      // for bulk inserts
      boost::container::static_vector<account_lookup, 127> heights{};
      static_assert(sizeof(heights) <= 1024, "stack vector is large");

      for (auto user = users.begin() ;; ++user)
      {
        if (heights.size() == heights.capacity() || user == users.end())
        {
          // bulk update account height index
          MONERO_CHECK(
            bulk_insert(*accounts_bh_cur, last_update, epee::to_span(heights))
          );
          if (user == users.end())
            break;
          heights.clear();
        }

        // faster to assume that account is still active
        account_status status_key = account_status::active;
        const account_id user_id = user->id();
        MDB_val key = lmdb::to_val(status_key);
        MDB_val value = lmdb::to_val(user_id);
        int err = mdb_cursor_get(accounts_cur.get(), &key, &value, MDB_GET_BOTH);
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          if (accounts_ba_cur == nullptr)
            MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));

          MDB_val temp_key = lmdb::to_val(by_address_version);
          MDB_val temp_value = lmdb::to_val(user->db_address());
          err = mdb_cursor_get(accounts_ba_cur.get(), &temp_key, &temp_value, MDB_GET_BOTH);
          if (err)
          {
            if (err != MDB_NOTFOUND)
              return {lmdb::error(err)};
            continue; // to next account
          }

          status_key =
            accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup)>(temp_value).value().status;
          MONERO_LMDB_CHECK(mdb_cursor_get(accounts_cur.get(), &key, &value, MDB_GET_BOTH));
        }
        expect<account> existing = accounts.get_value<account>(value);
        if (!existing || existing->scan_height != user->scan_height())
          continue; // to next account

        const block_id existing_height = existing->scan_height;

        existing->scan_height = block_id(last_update);
        value = lmdb::to_val(*existing);
        MONERO_LMDB_CHECK(mdb_cursor_put(accounts_cur.get(), &key, &value, MDB_CURRENT));

        heights.push_back(account_lookup{user->id(), status_key});

        key = lmdb::to_val(existing_height);
        value = lmdb::to_val(user_id);
        MONERO_LMDB_CHECK(mdb_cursor_get(accounts_bh_cur.get(), &key, &value, MDB_GET_BOTH));
        MONERO_LMDB_CHECK(mdb_cursor_del(accounts_bh_cur.get(), 0));

        MONERO_CHECK(bulk_insert(*outputs_cur, user->id(), epee::to_span(user->outputs())));
        MONERO_CHECK(add_spends(*spends_cur, *images_cur, user->id(), epee::to_span(user->spends())));

        MONERO_CHECK(check_hooks(*webhooks_cur, *events_cur, *user));
        MONERO_CHECK(
          add_ongoing_hooks(
            updated.second, *webhooks_cur, *outputs_cur, *events_cur, user->id(), block_id(first_new), block_id(last_update + 1)
          )
        );

        ++updated.first;
      } // ... for every account being updated ...
      return {std::move(updated)};
    });
  }

  expect<std::vector<subaddress_dict>>
  storage::upsert_subaddresses(const account_id id, const account_address& address, const crypto::secret_key& view_key, std::vector<subaddress_dict> subaddrs, const std::uint32_t max_subaddr)
  {
    MONERO_PRECOND(db != nullptr);
    std::sort(subaddrs.begin(), subaddrs.end());

    return db->try_write([this, id, &address, &view_key, &subaddrs, max_subaddr] (MDB_txn& txn) -> expect<std::vector<subaddress_dict>>
    {
      std::size_t subaddr_count = 0;
      std::vector<subaddress_dict> out{};
      index_ranges new_dict{};
      const auto add_out = [&out] (major_index major, index_range minor)
      {
        if (out.empty() || out.back().first != major)
          out.emplace_back(major, index_ranges{std::vector<index_range>{minor}});
        else
          out.back().second.get_container().push_back(minor);
      };

      const auto check_max_range = [&subaddr_count, max_subaddr] (const index_range& range) -> bool
      {
        const auto more = std::uint32_t(range[1]) - std::uint32_t(range[0]);
        if (max_subaddr - subaddr_count <= more)
          return false;
        subaddr_count += more + 1;
        return true;
      };
      const auto check_max_ranges = [&check_max_range] (const index_ranges& ranges) -> bool
      {
        for (const auto& range : ranges.get_container())
        {
          if (!check_max_range(range))
            return false;
        }
        return true;
      };

      cursor::subaddress_ranges   ranges_cur;
      cursor::subaddress_indexes  indexes_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.subaddress_ranges, ranges_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.subaddress_indexes, indexes_cur));

      MDB_val key = lmdb::to_val(id);
      MDB_val value{};
      int err = mdb_cursor_get(indexes_cur.get(), &key, &value, MDB_SET);
      if (err)
      {
        if (err != MDB_NOTFOUND)
          return {lmdb::error(err)};
      }
      else
      {
        MONERO_LMDB_CHECK(mdb_cursor_count(indexes_cur.get(), &subaddr_count));
        if (max_subaddr < subaddr_count)
          return {error::max_subaddresses};
      }

      for (auto& major_entry : subaddrs)
      {
        new_dict.get_container().clear();
        if (!check_subaddress_dict(major_entry))
        {
          MERROR("Invalid subaddress_dict given to storage::upsert_subaddrs");
          return {wire::error::schema::array};
        }

        value = lmdb::to_val(major_entry.first);
        err = mdb_cursor_get(ranges_cur.get(), &key, &value, MDB_GET_BOTH);
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          if (!check_max_ranges(major_entry.second))
            return {error::max_subaddresses};
          out.push_back(major_entry);
          new_dict = std::move(major_entry.second);
        }
        else // merge new minor index ranges with old
        {
          auto old_dict = subaddress_ranges.get_value(value);
          if (!old_dict)
            return old_dict.error();

          auto& old_range = old_dict->second.get_container();
          const auto& new_range = major_entry.second.get_container();

          auto old_loc = old_range.begin();
          auto new_loc = new_range.begin();
          for ( ; old_loc != old_range.end() && new_loc != new_range.end(); )
          {
            if (std::uint64_t(new_loc->at(1)) + 1 < std::uint32_t(old_loc->at(0)))
            { // new has no overlap with existing
              if (!check_max_range(*new_loc))
                return {error::max_subaddresses};

              new_dict.get_container().push_back(*new_loc);
              add_out(major_entry.first, *new_loc);
              ++new_loc;
            }
            else if (std::uint64_t(old_loc->at(1)) + 1 < std::uint32_t(new_loc->at(0)))
            { // existing has no overlap with new
              new_dict.get_container().push_back(*old_loc);
              ++old_loc;
            }
            else if (old_loc->at(0) <= new_loc->at(0) && new_loc->at(1) <= old_loc->at(1))
            { // new is completely within existing
              ++new_loc;
            }
            else // new overlap at beginning, end, or both
            {
              if (new_loc->at(0) < old_loc->at(0))
              { // overlap at beginning
                const index_range new_range{new_loc->at(0), minor_index(std::uint32_t(old_loc->at(0)) - 1)};
                if (!check_max_range(new_range))
                  return {error::max_subaddresses};
                add_out(major_entry.first, new_range);
                old_loc->at(0) = new_loc->at(0);
              }
              if (old_loc->at(1) < new_loc->at(1))
              { // overlap at end
                const index_range new_range{minor_index(std::uint32_t(old_loc->at(1)) + 1), new_loc->at(1)};
                if (!check_max_range(new_range))
                  return {error::max_subaddresses};
                add_out(major_entry.first, new_range);
                old_loc->at(1) = new_loc->at(1);
              }
              ++new_loc;
            }
          }

          std::copy(old_loc, old_range.end(), std::back_inserter(new_dict.get_container()));
          for ( ; new_loc != new_range.end(); ++new_loc)
          {
            if (!check_max_range(*new_loc))
              return {error::max_subaddresses};
            new_dict.get_container().push_back(*new_loc);
            add_out(major_entry.first, *new_loc);
          }
        }

        for (const auto& new_indexes : new_dict.get_container())
        {
          for (std::uint64_t minor : boost::counting_range(std::uint64_t(new_indexes[0]), std::uint64_t(new_indexes[1]) + 1))
          {
            subaddress_map new_value{};
            new_value.index = address_index{major_entry.first, minor_index(minor)};
            new_value.subaddress = new_value.index.get_spend_public(address, view_key);

            value = lmdb::to_val(new_value);

            MONERO_LMDB_CHECK(mdb_cursor_put(indexes_cur.get(), &key, &value, 0));
          }
        }

        const expect<epee::byte_slice> value_bytes =
          subaddress_ranges.make_value(major_entry.first, new_dict);
        if (!value_bytes)
          return value_bytes.error();
        value = MDB_val{value_bytes->size(), const_cast<void*>(static_cast<const void*>(value_bytes->data()))};
        MONERO_LMDB_CHECK(mdb_cursor_put(ranges_cur.get(), &key, &value, 0));
      }

      return {std::move(out)};
    });
  }

  expect<void> storage::add_webhook(const webhook_type type, const boost::optional<account_address>& address, const webhook_value& event)
  {
    if (event.second.url != "zmq")
    {
      epee::net_utils::http::url_content url{};
      if (event.second.url.empty() || !epee::net_utils::parse_url(event.second.url, url))
        return {error::bad_url};
      if (url.schema != "http" && url.schema != "https")
        return {error::bad_url};
    }

    return db->try_write([this, type, &address, &event] (MDB_txn& txn) -> expect<void>
    {
      cursor::accounts_by_address accounts_ba_cur;
      cursor::webhooks            webhooks_cur;

      MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
      MONERO_CHECK(check_cursor(txn, this->db->tables.webhooks, webhooks_cur));

      webhook_key key{account_id::invalid, type};
      MDB_val lmkey{};
      MDB_val lmvalue{};

      if (address)
      {
        lmkey = lmdb::to_val(by_address_version);
        lmvalue = lmdb::to_val(*address);
        const int err = mdb_cursor_get(accounts_ba_cur.get(), &lmkey, &lmvalue, MDB_GET_BOTH);
        if (err && err != MDB_NOTFOUND)
          return {lmdb::error(err)};
        if (err != MDB_NOTFOUND)
          key.user = MONERO_UNWRAP(accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup.id)>(lmvalue));
      }

      if (key.user == account_id::invalid && type == webhook_type::tx_confirmation)
        return {error::bad_webhook};

      lmkey = lmdb::to_val(key);
      const expect<epee::byte_slice> value = webhooks.make_value(event.first, event.second);
      if (!value)
        return value.error();
      lmvalue = MDB_val{value->size(), const_cast<void*>(static_cast<const void*>(value->data()))};
      MONERO_LMDB_CHECK(mdb_cursor_put(webhooks_cur.get(), &lmkey, &lmvalue, 0));
      return success();
    });
  }

  expect<void> storage::clear_webhooks(const epee::span<const account_address> addresses)
  {
     if (addresses.empty())
       return success();

     return db->try_write([this, addresses] (MDB_txn& txn) -> expect<void>
     {
       cursor::accounts_by_address accounts_ba_cur;
       cursor::webhooks            webhooks_cur;
       cursor::events              events_cur;

       MONERO_CHECK(check_cursor(txn, this->db->tables.accounts_ba, accounts_ba_cur));
       MONERO_CHECK(check_cursor(txn, this->db->tables.webhooks, webhooks_cur));
       MONERO_CHECK(check_cursor(txn, this->db->tables.events, events_cur));

       webhook_key key{account_id::invalid, webhook_type::tx_confirmation};
       for (const auto& address : addresses)
       {
         MDB_val lmkey = lmdb::to_val(by_address_version);
         MDB_val lmvalue = lmdb::to_val(address);

         MONERO_LMDB_CHECK(mdb_cursor_get(accounts_ba_cur.get(), &lmkey, &lmvalue, MDB_GET_BOTH));
         key.user = MONERO_UNWRAP(accounts_by_address.get_value<MONERO_FIELD(account_by_address, lookup.id)>(lmvalue));

         lmkey = lmdb::to_val(key);
         int err = mdb_cursor_get(webhooks_cur.get(), &lmkey, &lmvalue, MDB_SET);
         if (!err)
           MONERO_LMDB_CHECK(mdb_cursor_del(webhooks_cur.get(), MDB_NODUPDATA));

         lmkey = lmdb::to_val(key.user);
         err = mdb_cursor_get(events_cur.get(), &lmkey, &lmvalue, MDB_SET);
         if (!err)
           mdb_cursor_del(events_cur.get(), MDB_NODUPDATA);
       }

       return success();
     });
   }

   expect<void> storage::clear_webhooks(std::vector<boost::uuids::uuid> ids)
   {
     if (ids.empty())
       return success();

     std::sort(ids.begin(), ids.end());

     return db->try_write([this, &ids] (MDB_txn& txn) -> expect<void>
     {
       cursor::webhooks            webhooks_cur;
       cursor::events              events_cur;

       MONERO_CHECK(check_cursor(txn, this->db->tables.webhooks, webhooks_cur));
       MONERO_CHECK(check_cursor(txn, this->db->tables.events, events_cur));

       MDB_val key{};
       MDB_val value{};
       int err = mdb_cursor_get(webhooks_cur.get(), &key, &value, MDB_FIRST);
       for ( ; /* every webhook */ ; )
       {
         if (err)
         {
           if (err == MDB_NOTFOUND)
             break;
           return {lmdb::error(err)};
         }

         const boost::uuids::uuid id =
           MONERO_UNWRAP(webhooks.get_fixed_value<MONERO_FIELD(webhook_dupsort, event_id)>(value));
         if (std::binary_search(ids.begin(), ids.end(), id))
           MONERO_LMDB_CHECK(mdb_cursor_del(webhooks_cur.get(), 0));

         err = mdb_cursor_get(webhooks_cur.get(), &key, &value, MDB_NEXT);
       }

       err = mdb_cursor_get(events_cur.get(), &key, &value, MDB_FIRST);
       for ( ; /* every event */ ; )
       {
         if (err)
         {
           if (err == MDB_NOTFOUND)
             break;
           return {lmdb::error(err)};
         }

         const webhook_dupsort event =
           MONERO_UNWRAP(events_by_account_id.get_value<MONERO_FIELD(webhook_event, link_webhook)>(value));
         if (std::binary_search(ids.begin(), ids.end(), event.event_id))
           MONERO_LMDB_CHECK(mdb_cursor_del(events_cur.get(), 0));

         err = mdb_cursor_get(events_cur.get(), &key, &value, MDB_NEXT);
       }

       return success();
     });
   }
} // db
} // lws
