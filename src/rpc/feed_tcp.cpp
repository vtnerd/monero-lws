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

#include "feed.inl"

#include <cstring>

#include "db/account.h"
#include "db/string.h"
#include "error.h"
#include "rpc/light_wallet.h"
#include "rpc/login.h"
#include "wire/error.h"
#include "wire/json.h"
#include "wire/msgpack.h"

namespace lws { namespace rpc { namespace feed
{
  status map(const std::error_code error) noexcept
  {
    if (error == lws::error::blockchain_reorg)
      return status::blockchain_reorg;
    if (error == lws::error::daemon_timeout)
      return status::daemon_unresponsive;
    if (error == error::queue_max)
      return status::queue_error;
    if (error == error::missed_messages)
      return status::queue_error;
    if (error == lws::error::bad_address)
      return status::bad_address;
    if (error == lws::error::bad_view_key)
      return status::bad_view_key;
    if (error == lws::error::account_not_found)
      return status::account_not_found;
    if (error == error::protocol)
      return status::protocol_error;

    const std::error_category& category = error.category();
    if (category == wire::error::schema_category())
      return status::schema_error;
    if (category == wire::error::msgpack_category())
      return status::parse_error;
    if (category == wire::error::rapidjson_category())
      return status::parse_error;

    return status::unspecified_error;
  }

  const char* get_string(error value) noexcept
  {
    switch(value)
    {
    case error::missed_messages:
      return "Internal client thread missed internal ZeroMQ messages";
    case error::protocol:
      return "Client did not follow protocool";
    case error::queue_max:
      return "Max queue for outgoing push messages was reached";
    default:
      break;
    }
    return "unknown lws::rpc::feed::error";
  }

  namespace
  {
    class event_category : public boost::system::error_category
    {
    public:
      // Return a short descriptive name for the category
      virtual const char *name() const noexcept override final { return "lws::rpc::push::event"; }
      // Return what each enum means in text
      virtual std::string message(int c) const override final
      {
        switch(event(c))
        {
        case event::none:
          return "No error";
        case event::invalid:
          return "Client did not follow protocol";
        case event::timeout:
          return "Timeout on async operation";
        default:
          break;
        }
        return "unknown";
      }
      virtual boost::system::error_condition default_error_condition(int c) const noexcept override final
      {
        switch(event(c))
        {
        case event::timeout:
          return make_error_condition(boost::system::errc::timed_out);
        default:
          break;
        }
        return boost::system::error_condition(c, *this);
      }
    };

    struct category final : std::error_category
    {
      virtual const char* name() const noexcept override final
      {
        return "lws::rpc::push:error_category()";
      }

      virtual std::string message(int value) const override final
      {
        return get_string(lws::rpc::feed::error(value));
      }

      virtual std::error_condition default_error_condition(int value) const noexcept override final
      {
        switch (lws::rpc::feed::error(value))
        {
        case error::protocol:
          return std::errc::protocol_error;
        case error::queue_max:
          return std::errc::no_buffer_space;
        default:
          break; // map to unmatchable category
        }
        return std::error_condition{value, *this};
      }
    };
  } // anonymous

  const boost::system::error_category& get_event_category()
  {
    static event_category c;
    return c;
  }


  std::error_category const& error_category() noexcept
  {
    static const category instance{};
    return instance;
  }

  namespace
  {
    constexpr std::pair<boost::beast::string_view, protocol> get_map(const protocol proto) noexcept
    {
      return {get_string(proto), proto};
    }

    std::string get_string(const boost::beast::net::const_buffer buf)
    {
      return {reinterpret_cast<const char*>(buf.data()), buf.size()};
    }

    epee::byte_slice get_slice(const boost::beast::net::const_buffer buf)
    {
      return epee::byte_slice{{reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size()}};
    }

    template<typename T>
    expect<epee::byte_slice> prep(const T& src, const protocol proto)
    {
      epee::byte_stream sink{};

      const std::string_view prefix = T::prefix();
      sink.write({prefix.data(), prefix.size()});

      const std::error_code error = proto == protocol::v0_msgpack ?
        wire::msgpack::to_bytes(sink, src) : wire::json::to_bytes(sink, src);
      if (error)
        return error;
      return epee::byte_slice{std::move(sink)};
    }
  } // anonymous
   
  protocol get_protocol(const boost::beast::string_view list) noexcept
  {
    // in order of preference
    static constexpr const std::array<std::pair<boost::beast::string_view, protocol>, 2> supported
    {{
      get_map(protocol::v0_msgpack),
      get_map(protocol::v0_json)
    }};
    const boost::beast::http::token_list tokens{list};
    for (const auto token : tokens)
    {
      for (const auto support : supported)
      {
        if (support.first == token)
          return support.second;
      }
    }
    return protocol::invalid;
  }


  std::string_view get_prefix(const boost::beast::net::const_buffer buf)
  {
    char const* const begin = reinterpret_cast<const char*>(buf.data());
    void const* const match = std::memchr(begin, u8':', buf.size());
    if (match)
      return {begin, std::size_t(reinterpret_cast<const char*>(match) - begin + 1)};
    return {};
  }

  expect<epee::byte_slice> prep_error(const std::error_code error, const protocol proto)
  {
    return prep(feed_error{error}, proto);
  }

  expect<epee::byte_slice> prep_login(const db::storage& disk, account_sub* const sub, connection_sync* const sync, boost::beast::flat_buffer& buffer, const protocol proto)
  {
    LWS_VERIFY(sub && sync);
    const std::string_view prefix = get_prefix(buffer.cdata());
    if (prefix != feed_login::prefix())
      return {error::protocol};

    buffer.consume(prefix.size());
    feed_login login{};
    std::error_code error = proto == protocol::v0_msgpack ?
      wire::msgpack::from_bytes(get_slice(buffer.cdata()), login) :
      wire::json::from_bytes(get_string(buffer.cdata()), login);
    if (error)
      return error;

    /* Must sub to feed before starting db read or else lost txes could
    occur. If the client gets txes twice, it should merge via hash. */

    const expect<std::uint32_t> watcher = sub->watch(db::address_string(login.account.address));
    if (!watcher)
      return watcher.error();

    auto account = open_account(login.account, disk);
    if (!account)
      return account.error();

    sync->receives_only = login.receives_only;
    sync->id = account->first.id;
    sync->warnings = *watcher;
    if (!login.tx_sync)
    {
      const auto block = account->second.get_last_block();
      if (!block)
        return block.error();

      account->second.finish_read();
      const db::account& acct = account->first;
      return prep(feed_blocks{acct.start_height, acct.scan_height, block->id, acct.lookahead_fail, acct.lookahead}, proto);
    }

    auto txs = get_address_txs_response::load(account->second, account->first, true);
    if (!txs)
      return txs.error();
 
    account->second.finish_read();
    return prep(feed_tx_sync{std::move(*txs)}, proto);
  }

  //! Convert ZMQ_PUSH message from the scanner to websocket `update` message
  expect<epee::byte_slice> prep_update(const db::storage& disk, std::string&& source, connection_sync* const sync, const protocol proto)
  {
    LWS_VERIFY(sync);

    epee::byte_slice slice{std::move(source)};
    const std::string_view prefix = get_prefix({slice.data(), slice.size()});
    const bool warning = prefix == feed_warning::prefix();
    slice.remove_prefix(prefix.size());
    if (warning)
    {
      if (sync->receives_only)
        return epee::byte_slice{};

      feed_warning the_warning{};
      const std::error_code error = wire::msgpack::from_bytes(std::move(slice), the_warning);
      if (!error)
        return error;

      if (sync->warnings != the_warning.counter)
        return prep_error(error::missed_messages, proto);

      ++sync->warnings;
      if (the_warning.code == status::blockchain_reorg && sync->height != db::block_id::txpool)
        sync->height = std::min(sync->height, the_warning.height);

      return prep(the_warning, proto);
    }

    account::update from_scanner{};
    const std::error_code error = wire::msgpack::from_bytes(std::move(slice), from_scanner);
    if (error)
      return error;

    if (from_scanner.new_height != db::block_id::txpool)
    { 
      if (sync->height != db::block_id::txpool && sync->height != from_scanner.old_height)
        return prep_error(error::missed_messages, proto);
      sync->height = from_scanner.new_height;

      if (sync->receives_only && from_scanner.outputs.empty())
        return epee::byte_slice{};

      // suppress _some_ block messages when account is catching up
      if (!sync->on_tip && from_scanner.outputs.empty() && from_scanner.spends.empty() && std::chrono::steady_clock::now() - sync->last_update <= scanner_busy)
        return epee::byte_slice{};

      feed_blocks blocks{
        std::min(sync->last_reported, from_scanner.old_height),
        from_scanner.new_height,
        from_scanner.new_height
      };

      /* A read is unfortunate here, but it also catches accounts that have
        been moved to "hidden". The stream will immediately abort on error. */

      auto reader = disk.start_read();
      if (!reader)
        return reader.error();
      const auto last_block = reader->get_last_block();
      if (!last_block)
        return last_block.error();
      auto account = reader->get_account(db::account_status::active, sync->id);
      if (account == lws::error::account_not_found)
        account = reader->get_account(db::account_status::inactive, sync->id); // rare as scanner should not push updates
      if (!account)
        return account.error();
 
      blocks.blockchain_height = last_block->id;
      blocks.lookahead_fail = account->lookahead_fail;
      blocks.lookahead = account->lookahead;
      reader->finish_read();

      blocks.transactions =
        get_address_txs_response::load(std::move(from_scanner.outputs), std::move(from_scanner.spends));

      sync->on_tip = (to_uint(last_block->id) - to_uint(std::min(last_block->id, from_scanner.new_height))) <= on_tip;
      sync->last_update = std::chrono::steady_clock::now();
      sync->last_reported = from_scanner.new_height;
      return prep(blocks, proto);
    }
 
    // mempool
    LWS_VERIFY(from_scanner.outputs.size() == 1);
    return prep(feed_mempool{from_scanner.outputs.at(0)}, proto);
  } 

  bool start(boost::asio::ip::tcp::socket&& sock, boost::asio::io_context& io, const lws::rpc::client& client, const request& req, const lws::db::storage& disk, const std::chrono::seconds timeout)
  {
    return do_start(std::move(sock), io, client, req, disk, timeout);
  }
}}} // lws // rpc // feed 

