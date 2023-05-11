#pragma once

#include <utility>

#include "common/expect.h" // monero/src
#include "lmdb/error.h"    // monero/src
#include "lmdb/table.h"    // monero/src
#include "lmdb/util.h"     // monero/src
#include "wire/msgpack.h"

namespace lmdb
{
  //! Helper for grouping typical LMDB DBI options when key is fixed and value has msgpack component
  template<typename K, typename V1, typename V2>
  struct msgpack_table : table
  {
    using key_type = K;
    using fixed_value_type = V1;
    using msgpack_value_type = V2;
    using value_type = std::pair<fixed_value_type, msgpack_value_type>;

    constexpr explicit msgpack_table(const char* name, unsigned flags = 0, MDB_cmp_func value_cmp = nullptr) noexcept
      : table{name, flags, &lmdb::less<lmdb::native_type<K>>, value_cmp}
    {}

    static expect<key_type> get_key(MDB_val key)
    {
      if (key.mv_size != sizeof(key_type))
        return {lmdb::error(MDB_BAD_VALSIZE)};

      key_type out;
      std::memcpy(std::addressof(out), static_cast<char*>(key.mv_data), sizeof(out));
      return out;
    }

    static epee::byte_slice make_value(const fixed_value_type& val1, const msgpack_value_type& val2)
    {
      epee::byte_stream initial;
      initial.write({reinterpret_cast<const char*>(std::addressof(val1)), sizeof(val1)});
      return wire_write::to_bytes(wire::msgpack_slice_writer{std::move(initial), true}, val2);
    }

    /*!
        \tparam U must be same as `V`; used for sanity checking.
        \tparam F is the type within `U` that is being extracted.
        \tparam offset to `F` within `U`.

        \note If using `F` and `offset` to retrieve a specific field, use
            `MONERO_FIELD` macro in `src/lmdb/util.h` which calculates the
            offset automatically.

        \return Value of type `F` at `offset` within `value` which has
            type `U`.
    */
    template<typename U, typename F = U, std::size_t offset = 0>
    static expect<F> get_fixed_value(MDB_val value) noexcept
    {
      static_assert(std::is_same<U, V1>(), "bad MONERO_FIELD?");
      static_assert(std::is_pod<F>(), "F must be POD");
      static_assert(sizeof(F) + offset <= sizeof(U), "bad field type and/or offset");

      if (value.mv_size < sizeof(U))
        return {lmdb::error(MDB_BAD_VALSIZE)};

      F out;
      std::memcpy(std::addressof(out), static_cast<char*>(value.mv_data) + offset, sizeof(out));
      return out;
    }

    static expect<value_type> get_value(MDB_val value) noexcept
    {
      if (value.mv_size < sizeof(fixed_value_type))
        return {lmdb::error(MDB_BAD_VALSIZE)};
      std::pair<fixed_value_type, msgpack_value_type> out;
      std::memcpy(std::addressof(out.first), static_cast<const char*>(value.mv_data), sizeof(out.first));

      auto msgpack_bytes = lmdb::to_byte_span(value);
      msgpack_bytes.remove_prefix(sizeof(out.first));
      auto msgpack = wire::msgpack::from_bytes<msgpack_value_type>(epee::byte_slice{{msgpack_bytes}});
      if (!msgpack)
        return msgpack.error();
      out.second = std::move(*msgpack);

      return out;
    }
    
    //! Easier than doing another iterator .. for now :/
    static expect<std::vector<std::pair<key_type, std::vector<value_type>>>> get_all(MDB_cursor& cur)
    {
      MDB_val key{};
      MDB_val value{};
      int err = mdb_cursor_get(&cur, &key, &value, MDB_FIRST);
      std::vector<std::pair<key_type, std::vector<value_type>>> out;
      for ( ; /* for every key */ ; )
      {
        if (err)
        {
          if (err != MDB_NOTFOUND)
            return {lmdb::error(err)};
          break;
        }

        expect<key_type> next_key = get_key(key);
        if (!next_key)
          return next_key.error();
        out.emplace_back(std::move(*next_key), std::vector<value_type>{});

        for ( ; /* for every value at key */ ; )
        {
          if (err)
          {
            if (err != MDB_NOTFOUND)
              return {lmdb::error(err)};
            break;
          }
          expect<value_type> next_value = get_value(value);
          if (!next_value)
            return next_value.error();
          out.back().second.push_back(std::move(*next_value));
          err = mdb_cursor_get(&cur, &key, &value, MDB_NEXT_DUP);
        }

        err = mdb_cursor_get(&cur, &key, &value, MDB_NEXT_NODUP);
      } // every key
      return out;
    }
  };
} // lmdb
