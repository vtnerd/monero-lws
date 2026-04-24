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

#include "light_wallet.h"

#include <boost/range/adaptor/indexed.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "config.h"
#include "db/storage.h"
#include "db/string.h"
#include "error.h"
#include "lws_version.h"
#include "time_helper.h"       // monero/contrib/epee/include
#include "ringct/rctOps.h"     // monero/src
#include "rpc/feed.h"
#include "span.h"              // monero/contrib/epee/include
#include "util/random_outputs.h"
#include "version.h"           // monero/src
#include "wire.h"
#include "wire/adapted/crypto.h"
#include "wire/error.h"
#include "wire/json.h"
#include "wire/msgpack/read.h"
#include "wire/traits.h"
#include "wire/vector.h"
#include "wire/wrapper/array.h"
#include "wire/wrapper/defaulted.h"
#include "wire/wrappers_impl.h"

namespace
{
  using max_subaddrs = wire::max_element_count<16384>;

  enum class iso_timestamp : std::uint64_t {};

  struct rct_bytes
  {
    rct::key commitment;
    rct::key mask;
    rct::key amount;
  };
  static_assert(sizeof(rct_bytes) == 32 * 3, "padding in rct struct");

  struct expand_outputs
  {
    const std::pair<lws::db::output, std::vector<crypto::key_image>>& data;
    const crypto::secret_key& user_key;
  };
} // anonymous

namespace wire
{
  template<>
  struct is_blob<rct_bytes>
    : std::true_type
  {};
}

namespace
{
  void write_bytes(wire::json_writer& dest, const iso_timestamp self)
  {
    static_assert(std::is_integral<std::time_t>::value, "unexpected  time_t type");
    if (std::numeric_limits<std::time_t>::max() < std::uint64_t(self))
      throw std::runtime_error{"Exceeded max time_t value"};

    std::tm value;
    if (!epee::misc_utils::get_gmt_time(std::time_t(self), value))
      throw std::runtime_error{"Failed to convert std::time_t to std::tm"};

    char buf[21] = {0};
    if (sizeof(buf) - 1 != std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::addressof(value)))
      throw std::runtime_error{"strftime failed"};

    dest.string({buf, sizeof(buf) - 1});
  }

  void write_bytes(wire::json_writer& dest, const expand_outputs self)
  {
    /*! \TODO Sending the public key for the output isn't necessary, as it can be
      re-computed from the other parts. Same with the rct commitment and rct
      amount. Consider dropping these from the API after client upgrades. Not
      storing them in the DB saves 96-bytes per received out. */

    rct_bytes rct{};
    rct_bytes const* optional_rct = nullptr;
    const auto flags = unpack(self.data.first.extra).first;
    if (flags & lws::db::ringct_output)
    {
      if (!(flags & lws::db::coinbase_output))
      {
        crypto::key_derivation derived;
        if (!crypto::generate_key_derivation(self.data.first.spend_meta.tx_public, self.user_key, derived))
          MONERO_THROW(lws::error::crypto_failure, "generate_key_derivation failed");

        crypto::secret_key scalar;
        rct::ecdhTuple encrypted{self.data.first.ringct_mask, rct::d2h(self.data.first.spend_meta.amount)};

        crypto::derivation_to_scalar(derived, self.data.first.spend_meta.index, scalar);
        rct::ecdhEncode(encrypted, rct::sk2rct(scalar), false);

        rct.commitment = rct::commit(self.data.first.spend_meta.amount, self.data.first.ringct_mask);
        rct.mask = encrypted.mask;
        rct.amount = encrypted.amount;
      }
      else
        rct.mask = rct::identity();

      optional_rct = std::addressof(rct);
    }

    wire::object(dest,
      wire::field("amount", lws::rpc::safe_uint64(self.data.first.spend_meta.amount)),
      wire::field("public_key", self.data.first.pub),
      wire::field("index", self.data.first.spend_meta.index),
      wire::field("global_index", self.data.first.spend_meta.id.low),
      wire::field("tx_id", self.data.first.spend_meta.id.low),
      wire::field("tx_hash", std::cref(self.data.first.link.tx_hash)),
      wire::field("tx_prefix_hash", std::cref(self.data.first.tx_prefix_hash)),
      wire::field("tx_pub_key", self.data.first.spend_meta.tx_public),
      wire::field("timestamp", iso_timestamp(self.data.first.timestamp)),
      wire::field("height", self.data.first.link.height),
      wire::field("spend_key_images", std::cref(self.data.second)),
      wire::optional_field("rct", optional_rct),
      wire::field("recipient", std::cref(self.data.first.recipient))
    );
  }

  void convert_address(const boost::string_ref source, lws::db::account_address& dest)
  {
    expect<lws::db::account_address> bytes = lws::db::address_string(source);
    if (!bytes)
      WIRE_DLOG_THROW(wire::error::schema::fixed_binary, "invalid Monero address format - " << bytes.error());
    dest = std::move(*bytes);
  }
} // anonymous

namespace lws
{
  static void write_bytes(wire::json_writer& dest, random_output const& self)
  {
    const rct_bytes rct{self.keys.mask, rct::zero(), rct::zero()};
    wire::object(dest,
      wire::field("global_index", rpc::safe_uint64(self.index)),
      wire::field("public_key", std::cref(self.keys.key)),
      wire::field("rct", std::cref(rct))
    );
  }
  static void write_bytes(wire::json_writer& dest, random_ring const& self)
  {
    wire::object(dest,
      wire::field("amount", rpc::safe_uint64(self.amount)),
      wire::field("outputs", std::cref(self.ring))
    );
  };

  void rpc::read_bytes(wire::json_reader& source, safe_uint64& self)
  {
    self = safe_uint64(wire::integer::cast_unsigned<std::uint64_t>(source.safe_unsigned_integer()));
  }
  void rpc::write_bytes(wire::json_writer& dest, const safe_uint64 self)
  {
    auto buf = wire::json_writer::to_string(std::uint64_t(self));
    dest.string(buf.data());
  }
  void rpc::read_bytes(wire::json_reader& source, safe_uint64_array& self)
  {
    for (std::size_t count = source.start_array(0); !source.is_array_end(count); --count)
      self.values.emplace_back(wire::integer::cast_unsigned<std::uint64_t>(source.safe_unsigned_integer()));
    source.end_array();
  }

  void rpc::read_bytes(wire::reader& source, account_credentials& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.key))))
    );
    convert_address(address, self.address);
  }

  namespace rpc
  {
    daemon_status_response::daemon_status_response()
      : outgoing_connections_count(0),
        incoming_connections_count(0),
        height(0),
        network(lws::rpc::network_type(lws::config::network)),
        state(daemon_state::unavailable)
    {}

    namespace
    {
      constexpr const char* map_daemon_state[] = {"ok", "no_connections", "synchronizing", "unavailable"};
      constexpr const char* map_network_type[] = {"main", "test", "stage", "fake"};
    }
    WIRE_DEFINE_ENUM(daemon_state, map_daemon_state);
    WIRE_DEFINE_ENUM(network_type, map_network_type);
  }

  void rpc::write_bytes(wire::json_writer& dest, const daemon_status_response& self)
  {
    wire::object(dest,
      WIRE_FIELD(outgoing_connections_count),
      WIRE_FIELD(incoming_connections_count),
      WIRE_FIELD(height),
      WIRE_FIELD(target_height),
      WIRE_FIELD(network),
      WIRE_FIELD(state)
    );
  }

  rpc::feed_error::feed_error(std::error_code error)
    : msg(error.message()),
      code(feed::map(error))
  {}

  void rpc::write_bytes(wire::writer& dest, const feed_error& self)
  {
    wire::object(dest, WIRE_FIELD(msg), WIRE_FIELD(code));
  }

  void rpc::read_bytes(wire::reader& src, feed_login& self)
  {
    wire::object(src,
      WIRE_FIELD(account),
      WIRE_FIELD_DEFAULTED(tx_sync, true),
      WIRE_FIELD_DEFAULTED(receives_only, false)
    );
  }

  void rpc::write_bytes(wire::writer& dest, const feed_mempool& self)
  {
    epee::span<const std::uint8_t> const* payment_id = nullptr;
    epee::span<const std::uint8_t> payment_id_bytes;

    const auto extra = db::unpack(self.received.extra);
    if (extra.second)
    {
      payment_id = std::addressof(payment_id_bytes);

      if (extra.second == sizeof(self.received.payment_id.short_))
        payment_id_bytes = epee::as_byte_span(self.received.payment_id.short_);
      else
        payment_id_bytes = epee::as_byte_span(self.received.payment_id.long_);
    }

    rct::key const* optional_rct = nullptr;
    if (extra.first & lws::db::ringct_output)
      optional_rct = std::addressof(self.received.ringct_mask);

    wire::object(dest,
      wire::field("hash", std::cref(self.received.link.tx_hash)),
      wire::field("prefix_hash", std::cref(self.received.tx_prefix_hash)),
      wire::field("fee", self.received.fee),
      wire::field("unlock_time", self.received.unlock_time),
      wire::optional_field("payment_id", payment_id),
      wire::field("mixin", self.received.spend_meta.mixin_count),
      wire::field("amount", self.received.spend_meta.amount),
      wire::field("public_key", std::cref(self.received.pub)),
      wire::field("tx_pub_key", std::cref(self.received.spend_meta.tx_public)),
      wire::field("index", self.received.spend_meta.index),
      wire::optional_field("recipient", wire::defaulted(std::cref(self.received.recipient), db::address_index{})),
      wire::optional_field("rct", optional_rct)
    );
  }

  namespace
  {
    template<typename T>
    struct sync_wrapper
    {
      const T& data;
    };

    struct sync_transform_
    {
      template<typename T>
      sync_wrapper<T> operator()(const T& value) const noexcept
      { return {value}; }
    };
    constexpr const sync_transform_ sync_transform{};

    struct legacy_temp { db::output_id id; };
    struct composite_temp { db::output_id id; };

    bool operator!=(const composite_temp& left, const composite_temp& right) noexcept
    { return left.id != right.id; }
 
    void write_bytes(wire::writer& dest, const legacy_temp& self)
    {
      wire::object(dest,
        wire::field("amount", self.id.high),
        wire::field("index", self.id.low)
      );
    }

    void write_bytes(wire::writer& dest, const composite_temp& self)
    {
      wire::object(dest, wire::field("legacy", legacy_temp{self.id}));
    }

    void write_bytes(wire::writer& dest, sync_wrapper<db::output> self)
    {
      rct::key rct{};
      rct::key const* optional_rct = nullptr;
      const auto flags = unpack(self.data.extra).first;
      if (flags & lws::db::ringct_output)
      {
        if (!(flags & lws::db::coinbase_output))
          rct = self.data.ringct_mask;
        else
          rct = rct::identity();

        optional_rct = std::addressof(rct);
      }

      wire::object(dest,
        wire::field("amount", self.data.spend_meta.amount),
        wire::field("public_key", std::cref(self.data.pub)),
        wire::field("index", self.data.spend_meta.index),
        wire::optional_field("id", wire::defaulted(composite_temp{self.data.spend_meta.id}, composite_temp{db::output_id::txpool()})),
        wire::field("tx_pub_key", std::cref(self.data.spend_meta.tx_public)),
        wire::optional_field("rct", optional_rct),
        wire::optional_field("recipient", wire::defaulted(std::cref(self.data.recipient), db::address_index{}))
      );
    }

    void write_bytes(wire::writer& dest, sync_wrapper<rpc::transaction_spend> self)
    {
      wire::object(dest,
        wire::field("id", composite_temp{self.data.possible_spend.source}),
        wire::field("key_image", std::cref(self.data.possible_spend.image))
      );
    }

    void write_bytes(wire::writer& dest, sync_wrapper<rpc::get_transaction> self)
    {
      epee::span<const std::uint8_t> const* payment_id = nullptr;
      epee::span<const std::uint8_t> payment_id_bytes;

      const auto extra = db::unpack(self.data.info.extra);
      if (extra.second)
      {
        payment_id = std::addressof(payment_id_bytes);

        if (extra.second == sizeof(self.data.info.payment_id.short_))
          payment_id_bytes = epee::as_byte_span(self.data.info.payment_id.short_);
        else
          payment_id_bytes = epee::as_byte_span(self.data.info.payment_id.long_);
      }

      const bool is_coinbase = (extra.first & db::coinbase_output);
      const bool txpool = self.data.info.link.height == db::block_id::txpool;
      wire::object(dest,
        wire::field("hash", std::cref(self.data.info.link.tx_hash)),
        wire::field("prefix_hash", std::cref(self.data.info.tx_prefix_hash)),
        wire::field("timestamp", self.data.info.timestamp),
        wire::field("fee", self.data.info.fee),
        wire::field("unlock_time", self.data.info.unlock_time),
        wire::optional_field("height", wire::defaulted(self.data.info.link.height, db::block_id::txpool)),
        wire::optional_field("payment_id", payment_id),
        wire::optional_field("coinbase", wire::defaulted(is_coinbase, false)),
        wire::optional_field("mempool", wire::defaulted(txpool, false)),
        wire::field("mixin", self.data.info.spend_meta.mixin_count),
        wire::optional_field("spends", wire::array(boost::adaptors::transform(self.data.spends, sync_transform))),
        wire::optional_field("receives", wire::array(boost::adaptors::transform(self.data.receives, sync_transform)))
      );
    }
  }

  void rpc::write_bytes(wire::writer& dest, const feed_tx_sync& self)
  { 
    wire::object(dest,
      wire::field("scanned_block_height", self.info.scanned_block_height),
      wire::field("start_height", self.info.start_height),
      wire::field("blockchain_height", self.info.blockchain_height),
      wire::optional_field("lookahead_fail", wire::defaulted(self.info.lookahead_fail, unsigned(0))),
      wire::optional_field("lookahead", wire::defaulted(self.info.lookahead, db::address_index{})),
      wire::optional_field("transactions", wire::array(boost::adaptors::transform(self.info.transactions, sync_transform)))
    );
  }

  void rpc::write_bytes(wire::writer& dest, const feed_blocks& self)
  {
    wire::object(dest,
      WIRE_FIELD(scan_start),
      WIRE_FIELD(scan_end),
      WIRE_FIELD(blockchain_height),
      wire::optional_field("lookahead_fail", wire::defaulted(self.lookahead_fail, db::block_id(0))),
      wire::optional_field("lookahead", wire::defaulted(std::cref(self.lookahead), db::address_index{})),
      wire::optional_field("transactions", wire::array(boost::adaptors::transform(self.transactions, sync_transform)))
    );
  }

  rpc::feed_warning::feed_warning(std::error_code error, const std::uint32_t counter, const db::block_id height)
    : msg(error.message()),
      code(feed::map(error)),
      counter(counter),
      height(height)
  {}

  namespace
  {
    template<typename F, typename T>
    void map_feed_warning(F& format, T& self)
    {
      wire::object(format,
        WIRE_FIELD_ID(0, msg),
        WIRE_FIELD_ID(1, code),
        WIRE_FIELD_ID(2, counter),
        WIRE_FIELD_ID(3, height)
      );
    }
  }

  void rpc::read_bytes(wire::msgpack_reader& src, feed_warning& dest)
  { map_feed_warning(src, dest); }

  void rpc::write_bytes(wire::writer& dest, const feed_warning& src)
  { map_feed_warning(dest, src); }


  void rpc::write_bytes(wire::json_writer& dest, const new_subaddrs_response& self)
  {
    wire::object(dest, WIRE_FIELD(new_subaddrs), WIRE_FIELD(all_subaddrs));
  }


  void rpc::write_bytes(wire::json_writer& dest, const transaction_spend& self)
  {
    wire::object(dest,
      wire::field("amount", safe_uint64(self.meta.amount)),
      wire::field("key_image", std::cref(self.possible_spend.image)),
      wire::field("tx_pub_key", std::cref(self.meta.tx_public)),
      wire::field("out_index", self.meta.index),
      wire::field("mixin", self.possible_spend.mixin_count),
      wire::field("sender", std::cref(self.possible_spend.sender))
    );
  }

  void rpc::write_bytes(wire::json_writer& dest, const get_address_info_response& self)
  {
    wire::object(dest,
      WIRE_FIELD_COPY(locked_funds),
      WIRE_FIELD_COPY(total_received),
      WIRE_FIELD_COPY(total_sent),
      WIRE_FIELD_COPY(scanned_height),
      WIRE_FIELD_COPY(scanned_block_height),
      WIRE_FIELD_COPY(start_height),
      WIRE_FIELD_COPY(transaction_height),
      WIRE_FIELD_COPY(blockchain_height),
      WIRE_FIELD_DEFAULTED(lookahead_fail, unsigned(0)),
      WIRE_FIELD(spent_outputs),
      WIRE_OPTIONAL_FIELD(rates),
      WIRE_FIELD_DEFAULTED(lookahead, db::address_index{})
    );
  }

  namespace rpc
  {
    static void write_bytes(wire::json_writer& dest, boost::range::index_value<const get_transaction&> self)
    {
      epee::span<const std::uint8_t> const* payment_id = nullptr;
      epee::span<const std::uint8_t> payment_id_bytes;

      const auto extra = db::unpack(self.value().info.extra);
      if (extra.second)
      {
        payment_id = std::addressof(payment_id_bytes);

        if (extra.second == sizeof(self.value().info.payment_id.short_))
          payment_id_bytes = epee::as_byte_span(self.value().info.payment_id.short_);
        else
          payment_id_bytes = epee::as_byte_span(self.value().info.payment_id.long_);
      }

      const bool is_coinbase = (extra.first & db::coinbase_output);

      wire::object(dest,
        wire::field("id", std::uint64_t(self.index())),
        wire::field("hash", std::cref(self.value().info.link.tx_hash)),
        wire::field("timestamp", iso_timestamp(self.value().info.timestamp)),
        wire::field("total_received", safe_uint64(self.value().info.spend_meta.amount)),
        wire::field("total_sent", safe_uint64(self.value().spent)),
        wire::field("fee", safe_uint64(self.value().info.fee)),
        wire::field("unlock_time", self.value().info.unlock_time),
        wire::field("height", self.value().info.link.height),
        wire::optional_field("payment_id", payment_id),
        wire::field("coinbase", is_coinbase),
        wire::field("mempool", false),
        wire::field("mixin", self.value().info.spend_meta.mixin_count),
        wire::field("recipient", self.value().info.recipient),
        wire::field("spent_outputs", std::cref(self.value().spends))
      );
    }
  } // rpc

  std::vector<db::output::spend_meta_>::const_iterator
  rpc::get_address_txs_response::find_metadata(std::vector<db::output::spend_meta_> const& metas, const db::output_id id)
  {
    struct by_output_id
    {
      bool operator()(db::output::spend_meta_ const& left, db::output_id right) const noexcept
      {
        return left.id < right;
      }
      bool operator()(db::output_id left, db::output::spend_meta_ const& right) const noexcept
      {
        return left < right.id;
      }
    };
    return std::lower_bound(metas.begin(), metas.end(), id, by_output_id{});
  }

  namespace
  {
    template<typename T>
    struct vec_lmdb_
    {
      using iterator = typename std::vector<T>::const_iterator;
      using value_type = T;
 
      iterator pos;
      const iterator end;

      bool is_end() const { return pos == end; }
      vec_lmdb_& operator++()
      {
        ++pos;
        return *this;
      }

      template<typename U, typename G = U, std::size_t uoffset = 0>
      G get_value() const noexcept
      {
          static_assert(std::is_same<U, T>(), "bad MONERO_FIELD usage?");
          static_assert(std::is_trivially_copyable<U>(), "value type must be memcpy safe");
          static_assert(std::is_trivially_copyable<G>(), "field type must be memcpy safe");
          static_assert(sizeof(G) + uoffset <= sizeof(U), "bad field and/or offset");
          assert(sizeof(G) + uoffset <= end - pos);
          assert(!is_end());

          G value;
          std::memcpy(std::addressof(value), reinterpret_cast<const std::uint8_t*>(std::addressof(*pos)) + uoffset, sizeof(value));
          return value;
      }

      const value_type& operator*() const noexcept { return *pos; }
    };

    template<typename T>
    vec_lmdb_<T> vec_lmdb(const std::vector<T>& src) { return {src.begin(), src.end()}; }

    template<typename T, typename U>
    std::pair<std::vector<rpc::get_transaction>, std::uint64_t> merge_into_txes(T output, U spend, const std::size_t reserve, const bool all_outputs, const bool skip_spend_meta)
    {
      // merge input and output info into a single set of txes.

      std::uint64_t total = 0;
      std::vector<rpc::get_transaction> out{};
      std::vector<db::output::spend_meta_> metas{};

      out.reserve(reserve);
      metas.reserve(reserve);

      db::transaction_link next_output{};
      db::transaction_link next_spend{};

      if (!output.is_end())
        next_output = output.template get_value<MONERO_FIELD(db::output, link)>();
      if (!spend.is_end())
        next_spend = spend.template get_value<MONERO_FIELD(db::spend, link)>();

      while (!output.is_end() || !spend.is_end())
      {
        if (!out.empty())
        {
          db::transaction_link const& last = out.back().info.link;
          LWS_VERIFY((output.is_end() || last <= next_output) && (spend.is_end() || last <= next_spend));
        }

        if (spend.is_end() || (!output.is_end() && next_output <= next_spend))
        {
          LWS_VERIFY(!output.is_end());

          std::uint64_t amount = 0;
          if (out.empty() || out.back().info.link.tx_hash != next_output.tx_hash)
          {
            out.push_back({*output});
            amount = out.back().info.spend_meta.amount;
          }
          else
          {
            amount = output.template get_value<MONERO_FIELD(db::output, spend_meta.amount)>();
            out.back().info.spend_meta.amount += amount;
          }

          if (all_outputs)
            out.back().receives.push_back(*output);

          const db::output::spend_meta_ meta = output.template get_value<MONERO_FIELD(db::output, spend_meta)>();
          if (metas.empty() || metas.back().id < meta.id)
            metas.push_back(meta);
          else
            metas.insert(rpc::get_address_txs_response::find_metadata(metas, meta.id), meta);

          total += amount;

          ++output;
          if (!output.is_end())
            next_output = output.template get_value<MONERO_FIELD(db::output, link)>();
        }
        else if (output.is_end() || (next_spend < next_output))
        {
          using meta_type = db::output::spend_meta_;
          LWS_VERIFY(!spend.is_end());

          const db::output_id source_id = spend.template get_value<MONERO_FIELD(db::spend, source)>();
          const auto meta = rpc::get_address_txs_response::find_metadata(metas, source_id);
          LWS_VERIFY(skip_spend_meta || (meta != metas.end() && meta->id == source_id));

          if (out.empty() || out.back().info.link.tx_hash != next_spend.tx_hash)
          {
            out.push_back({});
            out.back().spends.push_back({skip_spend_meta ? meta_type{} : *meta, *spend});
            out.back().info.link.height = out.back().spends.back().possible_spend.link.height;
            out.back().info.link.tx_hash = out.back().spends.back().possible_spend.link.tx_hash;
            out.back().info.spend_meta.mixin_count =
              out.back().spends.back().possible_spend.mixin_count;
            out.back().info.timestamp = out.back().spends.back().possible_spend.timestamp;
            out.back().info.unlock_time = out.back().spends.back().possible_spend.unlock_time;
          }
          else
            out.back().spends.push_back({skip_spend_meta ? meta_type{} : *meta, *spend});

          out.back().spent += meta->amount;

          ++spend;
          if (!spend.is_end())
            next_spend = spend.template get_value<MONERO_FIELD(db::spend, link)>();
        }
      }
      
      return {std::move(out), total};
    }

    struct tx_link_sorter_
    { 
      template<typename T>
      bool operator()(const T& left, const T& right) const noexcept
      {
        return left.link < right.link;
      }
    };
    constexpr const tx_link_sorter_ by_tx_link{};
  }

  std::vector<rpc::get_transaction> rpc::get_address_txs_response::load(std::vector<db::output> outputs, std::vector<db::spend> spends)
  {
    std::sort(outputs.begin(), outputs.end(), by_tx_link);
    std::sort(spends.begin(), spends.end(), by_tx_link);
    return merge_into_txes(vec_lmdb(outputs), vec_lmdb(spends), outputs.size(), true, true).first;
  }

  expect<rpc::get_address_txs_response> rpc::get_address_txs_response::load(db::storage_reader& reader, const db::account& acct, const bool all_outputs)
  {
    auto outputs = reader.get_outputs(acct.id);
    if (!outputs)
      return outputs.error();

    auto spends = reader.get_spends(acct.id);
    if (!spends)
      return spends.error();

    const expect<db::block_info> last = reader.get_last_block();
    if (!last)
      return last.error();

    get_address_txs_response resp{};
    resp.scanned_height = std::uint64_t(acct.scan_height);
    resp.scanned_block_height = resp.scanned_height;
    resp.start_height = std::uint64_t(acct.start_height);
    resp.blockchain_height = std::uint64_t(last->id);
    resp.transaction_height = resp.blockchain_height;
    resp.lookahead_fail = db::to_uint(acct.lookahead_fail);
    resp.lookahead = acct.lookahead;

    auto out = merge_into_txes(outputs->make_iterator(), spends->make_iterator(), outputs->count(), all_outputs, false);
    resp.total_received = safe_uint64(out.second);
    resp.transactions = std::move(out.first);
    return resp;
  }

  void rpc::write_bytes(wire::json_writer& dest, const get_address_txs_response& self)
  {
    wire::object(dest,
      wire::field("total_received", safe_uint64(self.total_received)),
      WIRE_FIELD_COPY(scanned_height),
      WIRE_FIELD_COPY(scanned_block_height),
      WIRE_FIELD_COPY(start_height),
      WIRE_FIELD_COPY(transaction_height),
      WIRE_FIELD_COPY(blockchain_height),
      WIRE_FIELD_DEFAULTED(lookahead_fail, unsigned(0)),
      WIRE_FIELD_DEFAULTED(lookahead, db::address_index{}),
      wire::optional_field("transactions", wire::array(boost::adaptors::index(self.transactions)))
    );
  }

  void rpc::read_bytes(wire::json_reader& source, get_random_outs_request& self)
  {
    wire::object(source, WIRE_FIELD(count), WIRE_FIELD(amounts));
  }
  void rpc::write_bytes(wire::json_writer& dest, const get_random_outs_response& self)
  {
    wire::object(dest, WIRE_FIELD(amount_outs));
  }

  void rpc::write_bytes(wire::json_writer& dest, const get_subaddrs_response& self)
  {
    wire::object(dest, WIRE_FIELD(all_subaddrs));
  }

  void rpc::read_bytes(wire::json_reader& source, get_unspent_outs_request& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.creds.key)))),
      WIRE_FIELD(amount),
      WIRE_OPTIONAL_FIELD(mixin),
      WIRE_OPTIONAL_FIELD(use_dust),
      WIRE_OPTIONAL_FIELD(dust_threshold)
    );
    convert_address(address, self.creds.address);
  }
  void rpc::write_bytes(wire::json_writer& dest, const get_unspent_outs_response& self)
  {
    const auto expand = [&self] (const std::pair<db::output, std::vector<crypto::key_image>>& src)
    {
      return expand_outputs{src, self.user_key};
    };
    wire::object(dest,
      WIRE_FIELD_COPY(per_byte_fee),
      WIRE_FIELD_COPY(fee_mask),
      WIRE_FIELD_COPY(amount),
      WIRE_FIELD_DEFAULTED(lookahead_fail, unsigned(0)),
      wire::optional_field("outputs", wire::array(boost::adaptors::transform(self.outputs, expand))),
      WIRE_FIELD(fees)
    );
  }

  rpc::get_version_response::get_version_response(const db::block_id height, const std::uint32_t max_subaddresses)
    : server_type(lws::version::name),
      server_version(lws::version::id),
      last_git_commit_hash(lws::version::commit),
      last_git_commit_date(lws::version::date),
      git_branch_name(lws::version::branch),
      monero_version_full(MONERO_VERSION_FULL),
      blockchain_height(height),
      api(lws::version::api::combined),
      max_subaddresses(max_subaddresses),
      network(lws::rpc::network_type(lws::config::network)),
      testnet(config::network == cryptonote::TESTNET) 
  {}
  void rpc::write_bytes(wire::json_writer& dest, const get_version_response& self)
  {
    wire::object(dest,
      WIRE_FIELD(server_type),
      WIRE_FIELD(server_version),
      WIRE_FIELD(last_git_commit_hash),
      WIRE_FIELD(last_git_commit_date),
      WIRE_FIELD(git_branch_name),
      WIRE_FIELD(monero_version_full),
      WIRE_FIELD_COPY(blockchain_height),
      WIRE_FIELD(api),
      WIRE_FIELD_COPY(max_subaddresses),
      wire::field("network_type", self.network),
      WIRE_FIELD_COPY(testnet)
    );
  }

  void rpc::read_bytes(wire::json_reader& source, import_request& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.creds.key)))),
      WIRE_FIELD_DEFAULTED(from_height, unsigned(0)),
      WIRE_FIELD_DEFAULTED(lookahead, db::address_index{})
    );
    convert_address(address, self.creds.address);
  }

  void rpc::write_bytes(wire::json_writer& dest, const import_response& self)
  {
    wire::object(dest,
      WIRE_FIELD_COPY(import_fee),
      WIRE_FIELD(status),
      WIRE_FIELD_COPY(new_request),
      WIRE_FIELD_COPY(request_fulfilled),
      WIRE_FIELD_COPY(lookahead)
    );
  }

  void rpc::read_bytes(wire::json_reader& source, login_request& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.creds.key)))),
      WIRE_FIELD_DEFAULTED(lookahead, db::address_index{}),
      WIRE_FIELD(create_account),
      WIRE_FIELD(generated_locally)
    );
    convert_address(address, self.creds.address);
  }
  void rpc::write_bytes(wire::json_writer& dest, const login_response self)
  {
    wire::object(dest,
      WIRE_FIELD_COPY(new_address),
      WIRE_FIELD_COPY(generated_locally),
      WIRE_FIELD_COPY(lookahead)
    );
  }

  void rpc::read_bytes(wire::json_reader& source, provision_subaddrs_request& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.creds.key)))),
      WIRE_OPTIONAL_FIELD(maj_i),
      WIRE_OPTIONAL_FIELD(min_i),
      WIRE_OPTIONAL_FIELD(n_maj),
      WIRE_OPTIONAL_FIELD(n_min),
      WIRE_OPTIONAL_FIELD(get_all)
    );
    convert_address(address, self.creds.address);
  }
 
  void rpc::read_bytes(wire::json_reader& source, submit_raw_tx_request& self)
  {
    wire::object(source, WIRE_FIELD(tx));
  }
  void rpc::write_bytes(wire::json_writer& dest, const submit_raw_tx_response self)
  {
    wire::object(dest, WIRE_FIELD_COPY(status));
  }

  void rpc::read_bytes(wire::json_reader& source, upsert_subaddrs_request& self)
  {
    std::string address;
    wire::object(source,
      wire::field("address", std::ref(address)),
      wire::field("view_key", std::ref(unwrap(unwrap(self.creds.key)))),
      WIRE_FIELD_ARRAY(subaddrs, max_subaddrs),
      WIRE_OPTIONAL_FIELD(get_all)
    );
    convert_address(address, self.creds.address);
  }
} // lws
