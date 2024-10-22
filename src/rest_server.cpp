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
#include "rest_server.h"

#include <algorithm>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/optional/optional.hpp>
#include <boost/range/counting_range.hpp>
#include <boost/thread/tss.hpp>
#include <boost/utility/string_ref.hpp>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "common/error.h"          // monero/src
#include "common/expect.h"         // monero/src
#include "config.h"
#include "crypto/crypto.h"         // monero/src
#include "cryptonote_config.h"     // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "db/string.h"
#include "error.h"
#include "lmdb/util.h"             // monero/src
#include "net/net_parse_helpers.h" // monero/contrib/epee/include
#include "net/net_ssl.h"           // monero/contrib/epee/include
#include "net/zmq.h"               // monero/src
#include "net/zmq_async.h"
#include "rpc/admin.h"
#include "rpc/client.h"
#include "rpc/daemon_messages.h"   // monero/src
#include "rpc/light_wallet.h"
#include "rpc/rates.h"
#include "rpc/webhook.h"
#include "util/gamma_picker.h"
#include "util/random_outputs.h"
#include "util/source_location.h"
#include "wire/adapted/crypto.h"
#include "wire/json.h"

namespace lws
{
  namespace
  {
    namespace http = epee::net_utils::http;
    constexpr const std::size_t http_parser_buffer_size = 16 * 1024;
    constexpr const std::chrono::seconds zmq_reconnect_backoff{10};
    constexpr const std::chrono::seconds rest_handshake_timeout{5};
    constexpr const std::chrono::seconds rest_request_timeout{5};
    constexpr const std::chrono::seconds rest_response_timeout{15};

    //! `/daemon_status` and `get_unspent_outs` caches ZMQ result for this long
    constexpr const std::chrono::seconds daemon_cache_timeout{5};

    struct copyable_slice
    {
      epee::byte_slice value;

      copyable_slice(epee::byte_slice value) noexcept
        : value(std::move(value))
      {}

      copyable_slice(copyable_slice&&) = default;
      copyable_slice(const copyable_slice& rhs) noexcept
        : value(rhs.value.clone())
      {}

      copyable_slice& operator=(copyable_slice&&) = default;
      copyable_slice& operator=(const copyable_slice& rhs) noexcept
      {
        if (this != std::addressof(rhs))
          value = rhs.value.clone();
        return *this;
      }
    };
    using async_complete = void(expect<copyable_slice>);

    expect<rpc::client*> thread_client(const rpc::client& gclient, const bool reset = false)
    {
      struct tclient
      {
        rpc::client client;
        std::chrono::steady_clock::time_point last_connect;

        explicit tclient() noexcept
          : client(), last_connect(std::chrono::seconds{0})
        {}
      };
      static boost::thread_specific_ptr<tclient> global;

      tclient* thread_ptr = global.get();
      if (!thread_ptr)
      {
        thread_ptr = new tclient;
        global.reset(thread_ptr);
      }

      if (reset || !thread_ptr->client)
      {
        // This reduces ZMQ internal errors with lack of file descriptors
        const auto now = std::chrono::steady_clock::now();
        if (now - thread_ptr->last_connect < zmq_reconnect_backoff)
          return {error::daemon_timeout};

        // careful, gclient and thread_ptr->client could be aliased
        expect<rpc::client> new_client = gclient.clone();
        thread_ptr->client = rpc::client{};
        thread_ptr->last_connect = now;
        if (!new_client)
          return new_client.error();

        thread_ptr->client = std::move(*new_client);
      }

      return {std::addressof(thread_ptr->client)};
    }

    expect<void> send_with_retry(rpc::client& tclient, epee::byte_slice message, const std::chrono::seconds timeout)
    {
      expect<void> resp{common_error::kInvalidArgument};
      for (unsigned i = 0; i < 2; ++i)
      {
        resp = tclient.send(message.clone(), timeout);
        if (resp || resp != net::zmq::make_error_code(EFSM))
          break;
        // fix state machine by reading+discarding previously timed out response
        auto read = tclient.get_message(timeout);
        if (!read)
        {
          // message could've been delivered, then dropped in process failure
          thread_client(tclient, true);
          return read.error();
        }
      }
      return resp;
    }

    bool is_locked(std::uint64_t unlock_time, db::block_id last) noexcept
    {
      if (unlock_time > CRYPTONOTE_MAX_BLOCK_NUMBER)
        return std::chrono::seconds{unlock_time} > std::chrono::system_clock::now().time_since_epoch();
      return db::block_id(unlock_time) > last;
    }

    std::vector<db::output::spend_meta_>::const_iterator
    find_metadata(std::vector<db::output::spend_meta_> const& metas, db::output_id id)
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

    bool is_hidden(db::account_status status) noexcept
    {
      switch (status)
      {
      case db::account_status::active:
      case db::account_status::inactive:
        return false;
      default:
      case db::account_status::hidden:
        break;
      }
      return true;
    }

    bool key_check(const rpc::account_credentials& creds)
    {
      crypto::public_key verify{};
      if (!crypto::secret_key_to_public_key(creds.key, verify))
        return false;
      if (verify != creds.address.view_public)
        return false;
      return true;
    }

    //! \return Account info from the DB, iff key matches address AND address is NOT hidden.
    expect<std::pair<db::account, db::storage_reader>> open_account(const rpc::account_credentials& creds, db::storage disk)
    {
      if (!key_check(creds))
        return {lws::error::bad_view_key};

      auto reader = disk.start_read();
      if (!reader)
        return reader.error();

      const auto user = reader->get_account(creds.address);
      if (!user)
        return user.error();
      if (is_hidden(user->first))
        return {lws::error::account_not_found};
      return {std::make_pair(user->second, std::move(*reader))};
    }

    //! For endpoints that _sometimes_ generate async responses
    expect<epee::byte_slice> async_ready() noexcept
    { return epee::byte_slice{}; }

    //! Helper for `call` function when handling an _always_ async endpoint
    expect<epee::byte_slice> json_response(const expect<void>&) noexcept
    { return epee::byte_slice{}; }

    //! Helper for `call` function when handling a _sometimes_ async endpoint
    expect<epee::byte_slice> json_response(expect<epee::byte_slice> source) noexcept
    { return source; }

    //! Immediately generate JSON from `source`
    template<typename T>
    expect<epee::byte_slice> json_response(const T& source)
    {
      std::error_code error{};
      epee::byte_slice out{};
      if ((error = wire::json::to_bytes(out, source)))
        return error;
      return {std::move(out)};
    }

    //! Helper for `call` function when handling a _never_ async endpoint
    template<typename T>
    expect<epee::byte_slice> json_response(const expect<T>& source)
    { return json_response(source.value()); }

    std::atomic_flag rates_error_once = ATOMIC_FLAG_INIT;

    struct runtime_options
    {
      std::uint32_t max_subaddresses;
      epee::net_utils::ssl_verification_t webhook_verify;
      bool disable_admin_auth;
      bool auto_accept_creation;
    };

    struct rest_server_data
    {
      const db::storage disk;
      const rpc::client client;
      const runtime_options options;

      std::vector<net::zmq::async_client> clients;
      boost::mutex sync;

      expect<net::zmq::async_client> get_async_client(boost::asio::io_service& io)
      {
        boost::unique_lock<boost::mutex> lock{sync};
        if (!clients.empty())
        {
          net::zmq::async_client out{std::move(clients.back())};
          clients.pop_back();
          return out;
        }
        lock.unlock();
        return client.make_async_client(io);
      }

      void store_async_client(net::zmq::async_client&& client)
      {
        const boost::lock_guard<boost::mutex> lock{sync};
        client.close = false;
        clients.push_back(std::move(client));
      }
    };

    struct daemon_status
    {
      using request = rpc::daemon_status_request;
      using response = epee::byte_slice; // sometimes async
      using async_response = rpc::daemon_status_response;

      static expect<response> handle(const request&, boost::asio::io_service& io, rest_server_data& data, std::function<async_complete> resume)
      {
        using info_rpc = cryptonote::rpc::GetInfo;

        struct frame
        {
          rest_server_data* parent;
          epee::byte_slice out;
          std::string in;
          net::zmq::async_client client;
          boost::asio::steady_timer timer;
          boost::asio::io_service::strand strand;
          std::vector<std::function<async_complete>> resumers;

          frame(rest_server_data& parent, boost::asio::io_service& io, net::zmq::async_client client)
            : parent(std::addressof(parent)),
              out(),
              in(),
              client(std::move(client)),
              timer(io),
              strand(io),
              resumers()
          {
            info_rpc::Request daemon_req{};
            out = rpc::client::make_message("get_info", daemon_req);
          }
        };

        struct cached_result
        {
          std::weak_ptr<frame> status;
          epee::byte_slice result;
          std::chrono::steady_clock::time_point last;
          boost::mutex sync;

          cached_result() noexcept
            : status(), result(), last(std::chrono::seconds{0}), sync()
          {}
        };

        static cached_result cache;
        boost::unique_lock<boost::mutex> lock{cache.sync};

        if (!cache.result.empty() && std::chrono::steady_clock::now() - cache.last < daemon_cache_timeout)
          return cache.result.clone();

        auto active = cache.status.lock();
        if (active)
        {
          active->resumers.push_back(std::move(resume));
          return async_ready();
        }

        struct async_handler : public boost::asio::coroutine
        {
          std::shared_ptr<frame> self_;

          explicit async_handler(std::shared_ptr<frame> self)
            : boost::asio::coroutine(), self_(std::move(self))
          {}

          void send_response(const boost::system::error_code error, const expect<copyable_slice>& value)
          {
            assert(self_ != nullptr);

            if (error)
              MERROR("Failure in /daemon_status: " << error.message());
            else
            {
              // only re-use REQ socket if in proper state
              MDEBUG("Completed ZMQ request in /daemon_status");
              self_->parent->store_async_client(std::move(self_->client));
            }

            std::vector<std::function<async_complete>> resumers;
            {
              const boost::lock_guard<boost::mutex> lock{cache.sync};
              cache.status.reset(); // prevent more resumers being added
              resumers.swap(self_->resumers);
              if (value)
              {
                cache.result = value->value.clone();
                cache.last = std::chrono::steady_clock::now();
              }
              else
                cache.result = nullptr; // serialization error
            }

            // send default constructed response if I/O `error`
            for (const auto& r : resumers)
              r(value);
          }

          bool set_timeout(std::chrono::steady_clock::duration timeout, const bool expecting) const
          {
            struct on_timeout
            {
              std::shared_ptr<frame> self_;

              void operator()(boost::system::error_code error) const
              {
                if (!self_ || error == boost::asio::error::operation_aborted)
                  return;

                MWARNING("Timeout on /daemon_status ZMQ call");
                self_->client.close = true;
                self_->client.asock->cancel(error);
              }
            };

            assert(self_ != nullptr);
            if (!self_->timer.expires_after(timeout) && expecting)
              return false;

            self_->timer.async_wait(self_->strand.wrap(on_timeout{self_}));
            return true;
          }

          void operator()(boost::system::error_code error = {}, const std::size_t bytes = 0)
          {
            if (!self_)
              return;
            if (error)
              return send_response(error, json_response(async_response{}));

            frame& self = *self_;
            BOOST_ASIO_CORO_REENTER(*this)
            {
              set_timeout(std::chrono::seconds{2}, false);
              BOOST_ASIO_CORO_YIELD net::zmq::async_write(
                self.client, std::move(self.out), self.strand.wrap(std::move(*this))
              );

              if (!set_timeout(std::chrono::seconds{5}, true))
                return send_response(boost::asio::error::operation_aborted, json_response(async_response{}));

              BOOST_ASIO_CORO_YIELD net::zmq::async_read(
                self.client, self.in, self.strand.wrap(std::move(*this))
              );

              if (!self.timer.cancel(error))
                return send_response(boost::asio::error::operation_aborted, json_response(async_response{}));

              {
                info_rpc::Response daemon_resp{};
                const expect<void> status =
                  rpc::parse_response(daemon_resp, std::move(self.in));
                if (!status)
                  return send_response({}, status.error());

                async_response resp{};

                resp.outgoing_connections_count = daemon_resp.info.outgoing_connections_count;
                resp.incoming_connections_count = daemon_resp.info.incoming_connections_count;
                resp.height = daemon_resp.info.height;
                resp.target_height = daemon_resp.info.target_height;

                if (!resp.outgoing_connections_count && !resp.incoming_connections_count)
                  resp.state = rpc::daemon_state::no_connections;
                else if (resp.target_height && (resp.target_height - resp.height) >= 5)
                  resp.state = rpc::daemon_state::synchronizing;
                else
                  resp.state = rpc::daemon_state::ok;

                send_response({}, json_response(std::move(resp)));
              }
            }
          }
        };

        expect<net::zmq::async_client> client = data.get_async_client(io);
        if (!client)
          return client.error();

        active = std::make_shared<frame>(data, io, std::move(*client));
        cache.result = nullptr;
        cache.status = active;
        active->resumers.push_back(std::move(resume));
        lock.unlock();

        MDEBUG("Starting new ZMQ request in /daemon_status");
        active->strand.dispatch(async_handler{active});
        return async_ready();
      }
    };

    struct get_address_info
    {
      using request = rpc::account_credentials;
      using response = rpc::get_address_info_response;

      static expect<response> handle(const request& req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        auto user = open_account(req, data.disk.clone());
        if (!user)
          return user.error();

        response resp{};

        auto outputs = user->second.get_outputs(user->first.id);
        if (!outputs)
          return outputs.error();

        auto spends = user->second.get_spends(user->first.id);
        if (!spends)
          return spends.error();

        const expect<db::block_info> last = user->second.get_last_block();
        if (!last)
          return last.error();

        resp.blockchain_height = std::uint64_t(last->id);
        resp.transaction_height = resp.blockchain_height;
        resp.scanned_height = std::uint64_t(user->first.scan_height);
        resp.scanned_block_height = resp.scanned_height;
        resp.start_height = std::uint64_t(user->first.start_height);

        std::vector<db::output::spend_meta_> metas{};
        metas.reserve(outputs->count());

        for (auto output = outputs->make_iterator(); !output.is_end(); ++output)
        {
          const db::output::spend_meta_ meta =
            output.get_value<MONERO_FIELD(db::output, spend_meta)>();

          // these outputs will usually be in correct order post ringct
          if (metas.empty() || metas.back().id < meta.id)
            metas.push_back(meta);
          else
            metas.insert(find_metadata(metas, meta.id), meta);

          resp.total_received = rpc::safe_uint64(std::uint64_t(resp.total_received) + meta.amount);
          if (is_locked(output.get_value<MONERO_FIELD(db::output, unlock_time)>(), last->id))
            resp.locked_funds = rpc::safe_uint64(std::uint64_t(resp.locked_funds) + meta.amount);
        }

        resp.spent_outputs.reserve(spends->count());
        for (auto const& spend : spends->make_range())
        {
          const auto meta = find_metadata(metas, spend.source);
          if (meta == metas.end() || meta->id != spend.source)
          {
            throw std::logic_error{
              "Serious database error, no receive for spend"
            };
          }

          resp.spent_outputs.push_back({*meta, spend});
          resp.total_sent = rpc::safe_uint64(std::uint64_t(resp.total_sent) + meta->amount);
        }

        // `get_rates()` nevers does I/O, so handler can remain synchronous
        resp.rates = data.client.get_rates();
        if (!resp.rates && !rates_error_once.test_and_set(std::memory_order_relaxed))
          MWARNING("Unable to retrieve exchange rates: " << resp.rates.error().message());

        return resp;
      }
    };

    struct get_address_txs
    {
      using request = rpc::account_credentials;
      using response = rpc::get_address_txs_response;

      static expect<response> handle(const request& req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        auto user = open_account(req, data.disk.clone());
        if (!user)
          return user.error();

        auto outputs = user->second.get_outputs(user->first.id);
        if (!outputs)
          return outputs.error();

        auto spends = user->second.get_spends(user->first.id);
        if (!spends)
          return spends.error();

        const expect<db::block_info> last = user->second.get_last_block();
        if (!last)
          return last.error();

        response resp{};
        resp.scanned_height = std::uint64_t(user->first.scan_height);
        resp.scanned_block_height = resp.scanned_height;
        resp.start_height = std::uint64_t(user->first.start_height);
        resp.blockchain_height = std::uint64_t(last->id);
        resp.transaction_height = resp.blockchain_height;

        // merge input and output info into a single set of txes.

        auto output = outputs->make_iterator();
        auto spend = spends->make_iterator();

        std::vector<db::output::spend_meta_> metas{};

        resp.transactions.reserve(outputs->count());
        metas.reserve(resp.transactions.capacity());

        db::transaction_link next_output{};
        db::transaction_link next_spend{};

        if (!output.is_end())
          next_output = output.get_value<MONERO_FIELD(db::output, link)>();
        if (!spend.is_end())
          next_spend = spend.get_value<MONERO_FIELD(db::spend, link)>();

        while (!output.is_end() || !spend.is_end())
        {
          if (!resp.transactions.empty())
          {
            db::transaction_link const& last = resp.transactions.back().info.link;

            if ((!output.is_end() && next_output < last) || (!spend.is_end() && next_spend < last))
            {
              throw std::logic_error{"DB has unexpected sort order"};
            }
          }

          if (spend.is_end() || (!output.is_end() && next_output <= next_spend))
          {
            std::uint64_t amount = 0;
            if (resp.transactions.empty() || resp.transactions.back().info.link.tx_hash != next_output.tx_hash)
            {
              resp.transactions.push_back({*output});
              amount = resp.transactions.back().info.spend_meta.amount;
            }
            else
            {
              amount = output.get_value<MONERO_FIELD(db::output, spend_meta.amount)>();
              resp.transactions.back().info.spend_meta.amount += amount;
            }

            const db::output::spend_meta_ meta = output.get_value<MONERO_FIELD(db::output, spend_meta)>();
            if (metas.empty() || metas.back().id < meta.id)
              metas.push_back(meta);
            else
              metas.insert(find_metadata(metas, meta.id), meta);

            resp.total_received = rpc::safe_uint64(std::uint64_t(resp.total_received) + amount);

            ++output;
            if (!output.is_end())
              next_output = output.get_value<MONERO_FIELD(db::output, link)>();
          }
          else if (output.is_end() || (next_spend < next_output))
          {
            const db::output_id source_id = spend.get_value<MONERO_FIELD(db::spend, source)>();
            const auto meta = find_metadata(metas, source_id);
            if (meta == metas.end() || meta->id != source_id)
            {
              throw std::logic_error{
                "Serious database error, no receive for spend"
              };
            }

            if (resp.transactions.empty() || resp.transactions.back().info.link.tx_hash != next_spend.tx_hash)
            {
              resp.transactions.push_back({});
              resp.transactions.back().spends.push_back({*meta, *spend});
              resp.transactions.back().info.link.height = resp.transactions.back().spends.back().possible_spend.link.height;
              resp.transactions.back().info.link.tx_hash = resp.transactions.back().spends.back().possible_spend.link.tx_hash;
              resp.transactions.back().info.spend_meta.mixin_count =
                resp.transactions.back().spends.back().possible_spend.mixin_count;
              resp.transactions.back().info.timestamp = resp.transactions.back().spends.back().possible_spend.timestamp;
              resp.transactions.back().info.unlock_time = resp.transactions.back().spends.back().possible_spend.unlock_time;
            }
            else
              resp.transactions.back().spends.push_back({*meta, *spend});

            resp.transactions.back().spent += meta->amount;

            ++spend;
            if (!spend.is_end())
              next_spend = spend.get_value<MONERO_FIELD(db::spend, link)>();
          }
        }

        return resp;
      }
    };

    struct get_random_outs
    {
      using request = rpc::get_random_outs_request;
      using response = void; // always asynchronous response
      using async_response = rpc::get_random_outs_response;

      static expect<response> handle(request req, boost::asio::io_service& io, const rest_server_data& data, std::function<async_complete> resume)
      {
        using distribution_rpc = cryptonote::rpc::GetOutputDistribution;
        using histogram_rpc = cryptonote::rpc::GetOutputHistogram;
        using distribution_rpc = cryptonote::rpc::GetOutputDistribution;

        std::vector<std::uint64_t> amounts = std::move(req.amounts.values);

        if (50 < req.count || 20 < amounts.size())
          return {lws::error::exceeded_rest_request_limit};

        const expect<rpc::client*> tclient = thread_client(data.client);
        if (!tclient)
          return tclient.error();
        if (*tclient == nullptr)
          throw std::logic_error{"Unexpected rpc::client nullptr"};

        const std::greater<std::uint64_t> rsort{};
        std::sort(amounts.begin(), amounts.end(), rsort);
        const std::size_t ringct_count =
          amounts.end() - std::lower_bound(amounts.begin(), amounts.end(), 0, rsort);

        std::vector<lws::histogram> histograms{};
        if (ringct_count < amounts.size())
        {
          // reuse allocated vector memory
          amounts.resize(amounts.size() - ringct_count);

          histogram_rpc::Request histogram_req{};
          histogram_req.amounts = std::move(amounts);
          histogram_req.min_count = 0;
          histogram_req.max_count = 0;
          histogram_req.unlocked = true;
          histogram_req.recent_cutoff = 0;

          epee::byte_slice msg = rpc::client::make_message("get_output_histogram", histogram_req);
          MONERO_CHECK(send_with_retry(**tclient, std::move(msg), std::chrono::seconds{10}));

          auto histogram_resp = (*tclient)->receive<histogram_rpc::Response>(std::chrono::minutes{3}, MLWS_CURRENT_LOCATION);
          if (!histogram_resp)
            return histogram_resp.error();
          if (histogram_resp->histogram.size() != histogram_req.amounts.size())
            return {lws::error::bad_daemon_response};

          histograms = std::move(histogram_resp->histogram);

          amounts = std::move(histogram_req.amounts);
          amounts.insert(amounts.end(), ringct_count, 0);
        }

        std::vector<std::uint64_t> distributions{};
        if (ringct_count)
        {
          distribution_rpc::Request distribution_req{};
          if (ringct_count == amounts.size())
            distribution_req.amounts = std::move(amounts);

          distribution_req.amounts.resize(1);
          distribution_req.from_height = 0;
          distribution_req.to_height = 0;
          distribution_req.cumulative = true;

          epee::byte_slice msg =
            rpc::client::make_message("get_output_distribution", distribution_req);
          MONERO_CHECK(send_with_retry(**tclient, std::move(msg), std::chrono::seconds{10}));

          auto distribution_resp =
            (*tclient)->receive<distribution_rpc::Response>(std::chrono::minutes{3}, MLWS_CURRENT_LOCATION);
          if (!distribution_resp)
            return distribution_resp.error();

          if (distribution_resp->distributions.size() != 1)
            return {lws::error::bad_daemon_response};
          if (distribution_resp->distributions[0].amount != 0)
            return {lws::error::bad_daemon_response};

          distributions = std::move(distribution_resp->distributions[0].data.distribution);

          if (amounts.empty())
          {
            amounts = std::move(distribution_req.amounts);
            amounts.insert(amounts.end(), ringct_count - 1, 0);
          }
        }

        class zmq_fetch_keys
        {
          /* `std::function` needs a copyable functor. The functor was made
             const and copied in the function instead of using a reference to
             make the callback in `std::function` thread-safe. This shouldn't
             be a problem now, but this is just-in-case of a future refactor. */
          rpc::client* tclient;
        public:
          zmq_fetch_keys(rpc::client* src) noexcept
            : tclient(src)
          {}

          zmq_fetch_keys(zmq_fetch_keys&&) = default;
          zmq_fetch_keys(const zmq_fetch_keys&) = default;

          expect<std::vector<output_keys>> operator()(std::vector<lws::output_ref> ids) const
          {
            using get_keys_rpc = cryptonote::rpc::GetOutputKeys;
            if (tclient == nullptr)
              throw std::logic_error{"Unexpected nullptr in zmq_fetch_keys"};

            get_keys_rpc::Request keys_req{};
            keys_req.outputs = std::move(ids);

            epee::byte_slice msg = rpc::client::make_message("get_output_keys", keys_req);
            MONERO_CHECK(send_with_retry(*tclient, std::move(msg), std::chrono::seconds{10}));

            auto keys_resp = tclient->receive<get_keys_rpc::Response>(std::chrono::seconds{10}, MLWS_CURRENT_LOCATION);
            if (!keys_resp)
              return keys_resp.error();

            return {std::move(keys_resp->keys)};
          }
        };

        lws::gamma_picker pick_rct{std::move(distributions)};
        auto rings = pick_random_outputs(
          req.count,
          epee::to_span(amounts),
          pick_rct,
          epee::to_mut_span(histograms),
          zmq_fetch_keys{*tclient}
        );
        if (!rings)
          return rings.error();

        resume(json_response(async_response{std::move(*rings)}));
        return success();
      }
    };

    struct get_subaddrs
    {
      using request = rpc::account_credentials;
      using response = rpc::get_subaddrs_response;

      static expect<response> handle(request const& req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        auto user = open_account(req, data.disk.clone());
        if (!user)
          return user.error();
        auto subaddrs = user->second.get_subaddresses(user->first.id);
        if (!subaddrs)
          return subaddrs.error();
        return response{std::move(*subaddrs)};
      }
    };

    struct get_unspent_outs
    {
      using request = rpc::get_unspent_outs_request;
      using response = epee::byte_slice; // somtimes async response
      using async_response = rpc::get_unspent_outs_response;
      using rpc_command = cryptonote::rpc::GetFeeEstimate;

      static expect<response> generate_response(request req, const expect<rpc_command::Response>& rpc, db::storage disk)
      {
        if (!rpc)
          return rpc.error();

        auto user = open_account(req.creds, std::move(disk));
        if (!user)
          return user.error();

        if ((req.use_dust && *req.use_dust) || !req.dust_threshold)
          req.dust_threshold = rpc::safe_uint64(0);

        if (!req.mixin)
          req.mixin = 0;

        auto outputs = user->second.get_outputs(user->first.id);
        if (!outputs)
          return outputs.error();

        std::uint64_t received = 0;
        std::vector<std::pair<db::output, std::vector<crypto::key_image>>> unspent;

        unspent.reserve(outputs->count());
        for (db::output const& out : outputs->make_range())
        {
          if (out.spend_meta.amount < std::uint64_t(*req.dust_threshold) || out.spend_meta.mixin_count < *req.mixin)
            continue;

          received += out.spend_meta.amount;
          unspent.push_back({out, {}});

          auto images = user->second.get_images(out.spend_meta.id);
          if (!images)
            return images.error();

          unspent.back().second.reserve(images->count());
          auto range = images->make_range<MONERO_FIELD(db::key_image, value)>();
          std::copy(range.begin(), range.end(), std::back_inserter(unspent.back().second));
        }

        if (received < std::uint64_t(req.amount))
          return {lws::error::account_not_found};

        if (rpc->size_scale == 0 || 1024 < rpc->size_scale || rpc->fee_mask == 0)
          return {lws::error::bad_daemon_response};

        const std::uint64_t per_byte_fee =
          rpc->estimated_base_fee / rpc->size_scale;

        return json_response(
          async_response{
            per_byte_fee,
            rpc->fee_mask,
            rpc::safe_uint64(received),
            std::move(unspent),
            std::move(req.creds.key)
          }
        );
      }

      static expect<response> handle(request req, boost::asio::io_service& io, rest_server_data& data, std::function<async_complete> resume)
      {
        struct frame
        {
          rest_server_data* parent;
          epee::byte_slice out;
          std::string in;
          net::zmq::async_client client;
          boost::asio::steady_timer timer;
          boost::asio::io_service::strand strand;
          std::vector<std::pair<request, std::function<async_complete>>> resumers;

          frame(rest_server_data& parent, boost::asio::io_service& io, net::zmq::async_client client)
            : parent(std::addressof(parent)),
              out(),
              in(),
              client(std::move(client)),
              timer(io),
              strand(io),
              resumers()
          {
            rpc_command::Request req{};
            req.num_grace_blocks = 10;
            out = rpc::client::make_message("get_dynamic_fee_estimate", req);
          }
        };

        struct cached_result
        {
          std::weak_ptr<frame> status;
          rpc_command::Response result;
          std::chrono::steady_clock::time_point last;
          boost::mutex sync;

          cached_result() noexcept
            : status(), result{}, last(std::chrono::seconds{0}), sync()
          {}
        };

        static cached_result cache;
        boost::unique_lock<boost::mutex> lock{cache.sync};

        if (std::chrono::steady_clock::now() - cache.last < daemon_cache_timeout)
        {
          rpc_command::Response result = cache.result;
          lock.unlock();
          return generate_response(std::move(req), std::move(result), data.disk.clone());
        }

        auto active = cache.status.lock();
        if (active)
        {
          active->resumers.emplace_back(std::move(req), std::move(resume));
          return async_ready();
        }

        struct async_handler : public boost::asio::coroutine
        {
          std::shared_ptr<frame> self_;

          explicit async_handler(std::shared_ptr<frame> self)
            : boost::asio::coroutine(), self_(std::move(self))
          {}

          void send_response(const boost::system::error_code error, expect<rpc_command::Response> value)
          {
            assert(self_ != nullptr);

            if (error)
            {
              MERROR("Failure in /get_unspent_outs: " << error.message());
              value = {lws::error::daemon_timeout}; // old previous behavior
            }
            else
            {
              // only re-use REQ socket if in proper state
              MDEBUG("Completed ZMQ request in /get_unspent_outs");
              self_->parent->store_async_client(std::move(self_->client));
            }

            std::vector<std::pair<request, std::function<async_complete>>> resumers;
            {
              const boost::lock_guard<boost::mutex> lock{cache.sync};
              cache.status.reset(); // prevent more resumers being added
              resumers.swap(self_->resumers);
              if (value)
              {
                cache.result = *value;
                cache.last = std::chrono::steady_clock::now();
              }
              else
                cache.result = rpc_command::Response{};
            }

            // if `value` is error, it will return immediately
            for (auto& r : resumers)
              r.second(generate_response(std::move(r.first), value, self_->parent->disk.clone()));
          }

          bool set_timeout(std::chrono::steady_clock::duration timeout, const bool expecting) const
          {
            struct on_timeout
            {
              std::shared_ptr<frame> self_;

              void operator()(boost::system::error_code error) const
              {
                if (!self_ || error == boost::asio::error::operation_aborted)
                  return;

                MWARNING("Timeout on /get_unspent_outs ZMQ call");
                self_->client.close = true;
                self_->client.asock->cancel(error);
              }
            };

            assert(self_ != nullptr);
            if (!self_->timer.expires_after(timeout) && expecting)
              return false;

            self_->timer.async_wait(self_->strand.wrap(on_timeout{self_}));
            return true;
          }

          void operator()(boost::system::error_code error = {}, const std::size_t bytes = 0)
          {
            using default_response = rpc_command::Response;

            if (!self_)
              return;
            if (error)
              return send_response(error, default_response{});

            frame& self = *self_;
            BOOST_ASIO_CORO_REENTER(*this)
            {
              set_timeout(std::chrono::seconds{2}, false);
              BOOST_ASIO_CORO_YIELD net::zmq::async_write(
                self.client, std::move(self.out), self.strand.wrap(std::move(*this))
              );

              if (!set_timeout(std::chrono::seconds{5}, true))
                return send_response(boost::asio::error::operation_aborted, default_response{});

              BOOST_ASIO_CORO_YIELD net::zmq::async_read(
                self.client, self.in, self.strand.wrap(std::move(*this))
              );

              if (!self.timer.cancel(error))
                return send_response(boost::asio::error::operation_aborted, default_response{});

              {
                rpc_command::Response daemon_resp{};
                const expect<void> status =
                  rpc::parse_response(daemon_resp, std::move(self.in));
                if (!status)
                  return send_response({}, status.error());
                return send_response({}, std::move(daemon_resp));
              }
            }
          }
        };

        expect<net::zmq::async_client> client = data.get_async_client(io);
        if (!client)
          return client.error();

        active = std::make_shared<frame>(data, io, std::move(*client));
        cache.result = rpc_command::Response{};
        cache.status = active;
        active->resumers.emplace_back(std::move(req), std::move(resume));
        lock.unlock();

        MDEBUG("Starting new ZMQ request in /get_unspent_outs");
        active->strand.dispatch(async_handler{active});
        return async_ready();
      }
    };

    struct import_request
    {
      using request = rpc::account_credentials;
      using response = rpc::import_response;

      static expect<response> handle(request req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        bool new_request = false;
        bool fulfilled = false;
        {
          auto user = open_account(req, data.disk.clone());
          if (!user)
            return user.error();

          if (user->first.start_height == db::block_id(0))
            fulfilled = true;
          else
          {
            const expect<db::request_info> info =
              user->second.get_request(db::request::import_scan, req.address);

            if (!info)
            {
              if (info != lmdb::error(MDB_NOTFOUND))
                return info.error();

              new_request = true;
            }
          }
        } // close reader

        if (new_request)
          MONERO_CHECK(data.disk.clone().import_request(req.address, db::block_id(0)));

        const char* status = new_request ?
          "Accepted, waiting for approval" : (fulfilled ? "Approved" : "Waiting for Approval");
        return response{rpc::safe_uint64(0), status, new_request, fulfilled};
      }
    };

    struct login
    {
      using request = rpc::login_request;
      using response = rpc::login_response;

      static expect<response> handle(request req, boost::asio::io_service& io, const rest_server_data& data, std::function<async_complete> resume)
      {
        if (!key_check(req.creds))
          return {lws::error::bad_view_key};

        auto disk = data.disk.clone();
        {
          auto reader = disk.start_read();
          if (!reader)
            return reader.error();

          const auto account = reader->get_account(req.creds.address);
          reader->finish_read();

          if (account)
          {
            if (is_hidden(account->first))
              return {lws::error::account_not_found};

            // Do not count a request for account creation as login
            return response{false, bool(account->second.flags & db::account_generated_locally)};
          }
          else if (!req.create_account || account != lws::error::account_not_found)
            return account.error();
        }

        const auto flags = req.generated_locally ? db::account_generated_locally : db::default_account;
        const auto hooks = disk.creation_request(req.creds.address, req.creds.key, flags);
        if (!hooks)
          return hooks.error();

        if (data.options.auto_accept_creation)
        {
          const auto accepted = disk.accept_requests(db::request::create, {std::addressof(req.creds.address), 1});
          if (!accepted)
            MERROR("Failed to move account " << db::address_string(req.creds.address) << " to available state: " << accepted.error());
        }

        if (!hooks->empty())
        {
          // webhooks are not needed for response, so just queue i/o and
          // log errors when it fails

          rpc::send_webhook(
            data.client, epee::to_span(*hooks), "json-full-new_account_hook:", "msgpack-full-new_account_hook:", std::chrono::seconds{5}, data.options.webhook_verify
          );
        }

        return response{true, req.generated_locally};
      }
    };

    struct provision_subaddrs
    {
      using request = rpc::provision_subaddrs_request;
      using response = rpc::new_subaddrs_response;

      static expect<response> handle(request req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        if (!req.maj_i && !req.min_i && !req.n_min && !req.n_maj)
          return {lws::error::invalid_range};

        db::account_id id = db::account_id::invalid;
        {
          auto user = open_account(req.creds, data.disk.clone());
          if (!user)
            return user.error();
          id = user->first.id;
        }

        const std::uint32_t major_i = req.maj_i.value_or(0);
        const std::uint32_t minor_i = req.min_i.value_or(0);
        const std::uint32_t n_major = req.n_maj.value_or(50);
        const std::uint32_t n_minor = req.n_min.value_or(500);
        const bool get_all = req.get_all.value_or(true);
 
        std::vector<db::subaddress_dict> new_ranges;
        std::vector<db::subaddress_dict> all_ranges;
        if (n_major && n_minor)
        {
          if (std::numeric_limits<std::uint32_t>::max() / n_major < n_minor)
            return {lws::error::max_subaddresses};
          if (data.options.max_subaddresses < n_major * n_minor)
            return {lws::error::max_subaddresses};

          std::vector<db::subaddress_dict> ranges;
          ranges.reserve(n_major);
          for (std::uint64_t elem : boost::counting_range(std::uint64_t(major_i), std::uint64_t(major_i) + n_major))
          {
            ranges.emplace_back(
              db::major_index(elem), db::index_ranges{{db::index_range{db::minor_index(minor_i), db::minor_index(minor_i + n_minor - 1)}}}
            );
          }
          auto upserted = data.disk.clone().upsert_subaddresses(id, req.creds.address, req.creds.key, ranges, data.options.max_subaddresses);
          if (!upserted)
            return upserted.error();
          new_ranges = std::move(*upserted);
        }

        if (get_all)
        {
          // must start a new read after the last write
          auto disk = data.disk.clone();
          auto reader = disk.start_read();
          if (!reader)
            return reader.error();
          auto rc = reader->get_subaddresses(id);
          if (!rc)
            return rc.error();
          all_ranges = std::move(*rc);
        }
        return response{std::move(new_ranges), std::move(all_ranges)};
      }
    };

    struct submit_raw_tx
    {
      using request = rpc::submit_raw_tx_request;
      using response = void; // always async
      using async_response = rpc::submit_raw_tx_response;

      static expect<response> handle(request req, boost::asio::io_service& io, rest_server_data& data, std::function<async_complete> resume)
      {
        using transaction_rpc = cryptonote::rpc::SendRawTxHex;

        struct frame
        {
          rest_server_data* parent;
          std::string in;
          net::zmq::async_client client;
          boost::asio::steady_timer timer;
          boost::asio::io_service::strand strand;
          std::deque<std::pair<epee::byte_slice, std::function<async_complete>>> resumers;

          frame(rest_server_data& parent, boost::asio::io_service& io, net::zmq::async_client client)
            : parent(std::addressof(parent)),
              in(),
              client(std::move(client)),
              timer(io),
              strand(io),
              resumers()
          {}
        };

        struct cached_result
        {
          std::weak_ptr<frame> status;
          boost::mutex sync;

          cached_result() noexcept
            : status(), sync()
          {}
        };

        transaction_rpc::Request daemon_req{};
        daemon_req.relay = true;
        daemon_req.tx_as_hex = std::move(req.tx);

        epee::byte_slice msg = rpc::client::make_message("send_raw_tx_hex", daemon_req);

        static cached_result cache;
        boost::unique_lock<boost::mutex> lock{cache.sync};

        auto active = cache.status.lock();
        if (active)
        {
          active->resumers.emplace_back(std::move(msg), std::move(resume));
          return success();
        }

        struct async_handler : public boost::asio::coroutine
        {
          std::shared_ptr<frame> self_;

          explicit async_handler(std::shared_ptr<frame> self)
            : boost::asio::coroutine(), self_(std::move(self))
          {}

          void send_response(const boost::system::error_code error, expect<copyable_slice> value)
          {
            assert(self_ != nullptr);

            std::deque<std::pair<epee::byte_slice, std::function<async_complete>>> resumers;
            {
              const boost::lock_guard<boost::mutex> lock{cache.sync};
              if (error)
              {
                // Prevent further resumers, ZMQ REQ/REP in bad state
                MERROR("Failure in /submit_raw_tx: " << error.message());
                value = {lws::error::daemon_timeout};
                cache.status.reset();
                resumers.swap(self_->resumers);
              }
              else
              {
                MDEBUG("Completed ZMQ request in /submit_raw_tx");
                resumers.push_back(std::move(self_->resumers.front()));
                self_->resumers.pop_front();
              }
            }

            for (const auto& r : resumers)
              r.second(value);
          }

          bool set_timeout(std::chrono::steady_clock::duration timeout, const bool expecting) const
          {
            struct on_timeout
            {
              std::shared_ptr<frame> self_;

              void operator()(boost::system::error_code error) const
              {
                if (!self_ || error == boost::asio::error::operation_aborted)
                  return;

                MWARNING("Timeout on /submit_raw_tx ZMQ call");
                self_->client.close = true;
                self_->client.asock->cancel(error);
              }
            };

            assert(self_ != nullptr);
            if (!self_->timer.expires_after(timeout) && expecting)
              return false;

            self_->timer.async_wait(self_->strand.wrap(on_timeout{self_}));
            return true;
          }

          void operator()(boost::system::error_code error = {}, const std::size_t bytes = 0)
          {
            if (!self_)
              return;
            if (error)
              return send_response(error, async_ready());

            frame& self = *self_;
            epee::byte_slice next = nullptr;
            BOOST_ASIO_CORO_REENTER(*this)
            {
              for (;;)
              {
                {
                  const boost::lock_guard<boost::mutex> lock{cache.sync};
                  if (self_->resumers.empty())
                  {
                    cache.status.reset();
                    self_->parent->store_async_client(std::move(self_->client));
                    return;
                  }
                  next = std::move(self_->resumers.front().first);
                }

                set_timeout(std::chrono::seconds{10}, false);
                BOOST_ASIO_CORO_YIELD net::zmq::async_write(
                  self.client, std::move(next), self.strand.wrap(std::move(*this))
                );

                if (!set_timeout(std::chrono::seconds{20}, true))
                  return send_response(boost::asio::error::operation_aborted, async_ready());

                self.in.clear(); // could be in moved-from state
                BOOST_ASIO_CORO_YIELD net::zmq::async_read(
                  self.client, self.in, self.strand.wrap(std::move(*this))
                );

                if (!self.timer.cancel(error))
                  return send_response(boost::asio::error::operation_aborted, async_ready());

                {
                  transaction_rpc::Response daemon_resp{};
                  const expect<void> status =
                    rpc::parse_response(daemon_resp, std::move(self.in));

                  if (!status)
                    send_response({}, status.error());
                  else if (!daemon_resp.relayed)
                    send_response({}, {lws::error::tx_relay_failed});
                  else
                    send_response({}, json_response(async_response{"OK"}));
                }
              }
            }
          }
        };

        expect<net::zmq::async_client> client = data.get_async_client(io);
        if (!client)
          return client.error();

        active = std::make_shared<frame>(data, io, std::move(*client));
        cache.status = active;

        active->resumers.emplace_back(std::move(msg), std::move(resume));
        lock.unlock();

        MDEBUG("Starting new ZMQ request in /submit_raw_tx");
        active->strand.dispatch(async_handler{active});
        return success();
      }
    };

    struct upsert_subaddrs
    {
      using request = rpc::upsert_subaddrs_request;
      using response = rpc::new_subaddrs_response;

      static expect<response> handle(request req, const boost::asio::io_service&, const rest_server_data& data, std::function<async_complete>)
      {
        if (!data.options.max_subaddresses)
          return {lws::error::max_subaddresses};

        db::account_id id = db::account_id::invalid;
        {
          auto user = open_account(req.creds, data.disk.clone());
          if (!user)
            return user.error();
          id = user->first.id;
        }

        const bool get_all = req.get_all.value_or(true);

        std::vector<db::subaddress_dict> all_ranges;
        auto disk = data.disk.clone();
        auto new_ranges =
          disk.upsert_subaddresses(id, req.creds.address, req.creds.key, req.subaddrs, data.options.max_subaddresses);
        if (!new_ranges)
          return new_ranges.error();

        if (get_all)
        {
          auto reader = disk.start_read();
          if (!reader)
            return reader.error();
          auto rc = reader->get_subaddresses(id);
          if (!rc)
            return rc.error();
          all_ranges = std::move(*rc);
        }
        return response{std::move(*new_ranges), std::move(all_ranges)};
      }
    };

    template<typename E>
    expect<epee::byte_slice> call(std::string&& root, boost::asio::io_service& io, rest_server_data& data, std::function<async_complete> resume)
    {
      using request = typename E::request;
      using response = typename E::response;

      if (std::is_same<void, response>() && !resume)
        throw std::logic_error{"async REST handler not setup properly"};
      if (std::is_same<epee::byte_slice, response>() && !resume)
        throw std::logic_error{"async REST handler not setup properly"};

      request req{};
      std::error_code error = wire::json::from_bytes(std::move(root), req);
      if (error)
        return error;

      expect<response> resp = E::handle(std::move(req), io, data, std::move(resume));
      if (!resp)
        return resp.error();
      return json_response(std::move(resp));
    }

    template<typename T>
    struct admin
    {
      T params;
      boost::optional<crypto::secret_key> auth;
    };

    template<typename T>
    void read_bytes(wire::json_reader& source, admin<T>& self)
    {
        wire::object(source, WIRE_OPTIONAL_FIELD(auth), WIRE_FIELD(params));
    }
    void read_bytes(wire::json_reader& source, admin<expect<void>>& self)
    {
        // params optional
        wire::object(source, WIRE_OPTIONAL_FIELD(auth));
    }

    template<typename E>
    expect<epee::byte_slice> call_admin(std::string&& root, boost::asio::io_service&, rest_server_data& data, std::function<async_complete>)
    {
      using request = typename E::request;
      
      admin<request> req{};
      {
        const std::error_code error = wire::json::from_bytes(std::move(root), req);
        if (error)
          return error;
      }

      db::storage disk = data.disk.clone();
      if (!data.options.disable_admin_auth)
      {
        if (!req.auth)
          return {error::account_not_found};

        db::account_address address{};
        if (!crypto::secret_key_to_public_key(*(req.auth), address.view_public))
          return {error::crypto_failure};

        auto reader = disk.start_read();
        if (!reader)
          return reader.error();
        const auto account = reader->get_account(address);
        if (!account)
          return account.error();
        if (account->first == db::account_status::inactive)
          return {error::account_not_found};
        if (!(account->second.flags & db::account_flags::admin_account))
          return {error::account_not_found};
      }

      wire::json_slice_writer dest{};
      MONERO_CHECK(E{}(dest, std::move(disk), std::move(req.params)));
      return epee::byte_slice{dest.take_sink()};
    }

    struct endpoint
    {
      char const* const name;
      expect<epee::byte_slice> (*const run)(std::string&&, boost::asio::io_service&, rest_server_data&, std::function<async_complete>);
      const unsigned max_size;
      const bool is_async;
    };

    constexpr unsigned get_max(const endpoint* start, endpoint const* const end) noexcept
    {
      unsigned current_max = 0;
      for ( ; start != end; ++start)
        current_max = std::max(current_max, start->max_size);
      return current_max;
    }

    constexpr const endpoint endpoints[] =
    {
      {"/daemon_status",         call<daemon_status>,          1024,  true},
      {"/get_address_info",      call<get_address_info>,   2 * 1024, false},
      {"/get_address_txs",       call<get_address_txs>,    2 * 1024, false},
      {"/get_random_outs",       call<get_random_outs>,    2 * 1024,  true},
      {"/get_subaddrs",          call<get_subaddrs>,       2 * 1024, false},
      {"/get_txt_records",       nullptr,                         0, false},
      {"/get_unspent_outs",      call<get_unspent_outs>,   2 * 1024,  true},
      {"/import_wallet_request", call<import_request>,     2 * 1024, false},
      {"/login",                 call<login>,              2 * 1024, false},
      {"/provision_subaddrs",    call<provision_subaddrs>, 2 * 1024, false},
      {"/submit_raw_tx",         call<submit_raw_tx>,     50 * 1024,  true},
      {"/upsert_subaddrs",       call<upsert_subaddrs>,   10 * 1024, false}
    };
    constexpr const unsigned max_standard_endpoint_size =
      get_max(std::begin(endpoints), std::end(endpoints));

    constexpr const endpoint admin_endpoints[] =
    {
      {"/accept_requests",       call_admin<rpc::accept_requests_>, 50 * 1024, false},
      {"/add_account",           call_admin<rpc::add_account_>,     50 * 1024, false},
      {"/list_accounts",         call_admin<rpc::list_accounts_>,         100, false},
      {"/list_requests",         call_admin<rpc::list_requests_>,         100, false},
      {"/modify_account_status", call_admin<rpc::modify_account_>,  50 * 1024, false},
      {"/reject_requests",       call_admin<rpc::reject_requests_>, 50 * 1024, false},
      {"/rescan",                call_admin<rpc::rescan_>,          50 * 1024, false},
      {"/validate",              call_admin<rpc::validate_>,        50 * 1024, false},
      {"/webhook_add",           call_admin<rpc::webhook_add_>,     50 * 1024, false},
      {"/webhook_delete",        call_admin<rpc::webhook_delete_>,  50 * 1024, false},
      {"/webhook_delete_uuid",   call_admin<rpc::webhook_del_uuid_>,50 * 1024, false},
      {"/webhook_list",          call_admin<rpc::webhook_list_>,          100, false}
    };
    constexpr const unsigned max_admin_endpoint_size =
      get_max(std::begin(endpoints), std::end(endpoints));

    constexpr const unsigned max_endpoint_size =
      std::max(max_standard_endpoint_size, max_admin_endpoint_size);

    struct by_name_
    {
      bool operator()(endpoint const& left, endpoint const& right) const noexcept
      {
        if (left.name && right.name)
          return std::strcmp(left.name, right.name) < 0;
        return false;
      }
      bool operator()(const boost::beast::string_view left, endpoint const& right) const noexcept
      {
        if (right.name)
          return left < right.name;
        return false;
      }
      bool operator()(endpoint const& left, const boost::beast::string_view right) const noexcept
      {
        if (left.name)
          return left.name < right;
        return false;
      }
    };
    constexpr const by_name_ by_name{};

    struct slice_body
    {
      using value_type = epee::byte_slice;

      static std::size_t size(const value_type& source) noexcept
      {
        return source.size();
      }

      struct writer
      {
        epee::byte_slice body_;

        using const_buffers_type = boost::asio::const_buffer;

        template<bool is_request, typename Fields>
        explicit writer(boost::beast::http::header<is_request, Fields> const&, value_type const& body)
          : body_(body.clone())
        {}

        void init(boost::beast::error_code& ec)
        {
          ec = {};
        }

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::beast::error_code& ec)
        {
          ec = {};
          return {{const_buffers_type{body_.data(), body_.size()}, false}};
        }
      };
    };
  } // anonymous

  struct rest_server::internal
  {
    rest_server_data data;
    boost::optional<std::string> prefix;
    boost::optional<std::string> admin_prefix;
    boost::optional<boost::asio::ssl::context> ssl_;
    boost::asio::ip::tcp::acceptor acceptor;

    explicit internal(boost::asio::io_service& io_service, lws::db::storage disk, rpc::client client, runtime_options options)
      : data{std::move(disk), std::move(client), std::move(options)}
      , prefix()
      , admin_prefix()
      , ssl_()
      , acceptor(io_service)
    {
      assert(std::is_sorted(std::begin(endpoints), std::end(endpoints), by_name));
    }

    const endpoint* get_endpoint(boost::beast::string_view uri) const
    {
      using span = epee::span<const endpoint>;
      span handlers = nullptr;

      if (admin_prefix && uri.starts_with(*admin_prefix))
      {
        uri.remove_prefix(admin_prefix->size());
        handlers = span{admin_endpoints};
      }
      else if (prefix && uri.starts_with(*prefix))
      {
        uri.remove_prefix(prefix->size());
        handlers = span{endpoints};
      }
      else
        return nullptr;

      const auto handler = std::lower_bound(
        std::begin(handlers), std::end(handlers), uri, by_name
      );
      if (handler == std::end(handlers) || handler->name != uri)
        return nullptr;
      return handler;
    }
  };

  template<typename Sock>
  struct rest_server::connection
  {
    internal* parent_;
    Sock sock_;
    boost::beast::flat_static_buffer<http_parser_buffer_size> buffer_;
    boost::optional<boost::beast::http::parser<true, boost::beast::http::string_body>> parser_;
    boost::beast::http::response<slice_body> response_;
    boost::asio::steady_timer timer_;
    boost::asio::io_service::strand strand_;
    bool keep_alive_;

    static boost::asio::ip::tcp::socket make_socket(std::true_type, internal* parent)
    {
      return boost::asio::ip::tcp::socket{GET_IO_SERVICE(parent->acceptor)};
    }

    static boost::asio::ssl::stream<boost::asio::ip::tcp::socket> make_socket(std::false_type, internal* parent)
    {
      return boost::asio::ssl::stream<boost::asio::ip::tcp::socket>{
        GET_IO_SERVICE(parent->acceptor), parent->ssl_.value()
      };
    }

    static boost::asio::ip::tcp::socket& get_tcp(boost::asio::ip::tcp::socket& sock)
    {
      return sock;
    }

    static boost::asio::ip::tcp::socket& get_tcp(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& sock)
    {
      return sock.next_layer();
    }

    boost::asio::ip::tcp::socket& sock() { return get_tcp(sock_); }

    explicit connection(internal* parent) noexcept
      : parent_(parent),
        sock_(make_socket(std::is_same<Sock, boost::asio::ip::tcp::socket>(), parent)),
        buffer_{},
        parser_{},
        response_{},
        timer_(GET_IO_SERVICE(parent->acceptor)),
        strand_(GET_IO_SERVICE(parent->acceptor)),
        keep_alive_(true)
    {}

    ~connection()
    {
      MDEBUG("Destroying connection " << this);
    }

    template<typename F>
    void bad_request(const boost::beast::http::status status, F&& resume)
    {
      MDEBUG("Sending HTTP status " << int(status) << " to " << this);

      assert(strand_.running_in_this_thread());
      response_ = {status, parser_->get().version()};
      response_.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response_.keep_alive(keep_alive_);
      response_.prepare_payload();
      resume();
    }

    template<typename F>
    void bad_request(const std::error_code error, F&& resume)
    {
      boost::system::error_code ec{};
      MINFO("REST error: " << error.message() << " from " << sock().remote_endpoint(ec) << " / " << this);

      assert(strand_.running_in_this_thread());
      if (error.category() == wire::error::rapidjson_category() || error == lws::error::invalid_range)
        return bad_request(boost::beast::http::status::bad_request, std::forward<F>(resume));
      else if (error == lws::error::account_not_found || error == lws::error::duplicate_request)
        return bad_request(boost::beast::http::status::forbidden, std::forward<F>(resume));
      else if (error == lws::error::max_subaddresses)
        return bad_request(boost::beast::http::status::conflict, std::forward<F>(resume));
      else if (error.default_error_condition() == std::errc::timed_out || error.default_error_condition() == std::errc::no_lock_available)
        return bad_request(boost::beast::http::status::service_unavailable, std::forward<F>(resume));
      return bad_request(boost::beast::http::status::internal_server_error, std::forward<F>(resume));
    }

    template<typename F>
    void valid_request(epee::byte_slice body, F&& resume)
    {
      MDEBUG("Sending HTTP 200 OK (" << body.size() << " bytes) to " << this);

      assert(strand_.running_in_this_thread());
      response_ = {boost::beast::http::status::ok, parser_->get().version(), std::move(body)};
      response_.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response_.set(boost::beast::http::field::content_type, "application/json");
      response_.keep_alive(keep_alive_);
      response_.prepare_payload();
      resume(); // runs in strand
    }

    static bool set_timeout(const std::shared_ptr<connection>& self, const std::chrono::steady_clock::duration timeout, const bool existing)
    {
      if (!self)
        return false;

      struct on_timeout
      {
        std::shared_ptr<connection> self_;

        void operator()(boost::system::error_code error) const
        {
          if (!self_ || error == boost::asio::error::operation_aborted)
            return;

          MWARNING("Timeout on REST connection to " << self_->sock().remote_endpoint(error) << " / " << self_.get());
          self_->sock().cancel(error);
          self_->shutdown();
        }
      };

      if (!self->timer_.expires_after(timeout) && existing)
        return false; // timeout queued, just abort
      self->timer_.async_wait(self->strand_.wrap(on_timeout{self}));
      return true;
    }

    void shutdown()
    {
      boost::system::error_code ec{};
      MDEBUG("Shutting down REST socket to " << this);
      sock().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      timer_.cancel(ec);
    }
  };

  template<typename Sock>
  struct rest_server::handler_loop final : public boost::asio::coroutine
  {
    std::shared_ptr<connection<Sock>> self_;

    explicit handler_loop(std::shared_ptr<connection<Sock>> self) noexcept
      : boost::asio::coroutine(), self_(std::move(self))
    {}

    static void async_handshake(const boost::asio::ip::tcp::socket&) noexcept
    {}

    void async_handshake(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& sock)
    {
      connection<Sock>& self = *self_;
      self.sock_.async_handshake(
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket>::server,
        self.strand_.wrap(std::move(*this))
      );
    }

    template<typename F>
    void async_response(F&& resume)
    {
      assert(self_ != nullptr);
      assert(self_->strand_.running_in_this_thread());

      // checks access for `parser_` on first use
      self_->keep_alive_ = self_->parser_->get().keep_alive();
      const auto target = self_->parser_.value().get().target();

      MDEBUG("Received HTTP request from " << self_.get() << " to target " << target);

      // Checked access for `parser_` on first use
      endpoint const* const handler = self_->parent_->get_endpoint(target);
      if (!handler)
        return self_->bad_request(boost::beast::http::status::not_found, std::forward<F>(resume));

      if (handler->run == nullptr)
        return self_->bad_request(boost::beast::http::status::not_implemented, std::forward<F>(resume));

      const auto payload_size =
        self_->parser_->get().payload_size().value_or(std::numeric_limits<std::uint64_t>::max());
      if (handler->max_size < payload_size)
      {
        boost::system::error_code error{};
        MINFO("REST client (" << self_->sock().remote_endpoint(error) << " / " << self_.get() << ") exceeded maximum body size (" << handler->max_size << " bytes)");
        return self_->bad_request(boost::beast::http::status::bad_request, std::forward<F>(resume));
      }

      if (self_->parser_->get().method() != boost::beast::http::verb::post)
        return self_->bad_request(boost::beast::http::status::method_not_allowed, std::forward<F>(resume));

      std::function<async_complete> resumer;
      if (handler->is_async)
      {
        /* The `resumer` callback can be invoked in another strand (created
          by the handler function), and therefore needs to be "wrapped" to
          ensure thread safety. This also allows `resume` to be unwrapped. */
        const auto& self = self_;
        resumer = self->strand_.wrap(
          [self, resume] (expect<copyable_slice> body) mutable
          {
            if (!body)
              self->bad_request(body.error(), std::move(resume));
            else
              self->valid_request(std::move(body->value), std::move(resume));
          }
        );
      }

      MDEBUG("Running REST handler " << handler->name << " on " << self_.get());
      auto body = handler->run(std::move(self_->parser_->get()).body(), GET_IO_SERVICE(self_->timer_), self_->parent_->data, std::move(resumer));
      if (!body)
        return self_->bad_request(body.error(), std::forward<F>(resume));
      else if (!handler->is_async || !body->empty())
        return self_->valid_request(std::move(*body), std::forward<F>(resume));
      // else wait for `resumer` to continue response coroutine
      MDEBUG("REST response to " << self_.get() << " is being generated async");
    }

    void operator()(boost::system::error_code error = {}, const std::size_t bytes = 0)
    {
      using not_ssl = std::is_same<Sock, boost::asio::ip::tcp::socket>;

      if (!self_)
        return;

      assert(self_->strand_.running_in_this_thread());
      if (error)
      {
        boost::system::error_code ec{};
        if (error != boost::asio::error::operation_aborted && error != boost::beast::http::error::end_of_stream)
          MERROR("Error on REST socket (" << self_->sock().remote_endpoint(ec) << " / " << self_.get() << "): " << error.message());
        return self_->shutdown();
      }

      connection<Sock>& self = *self_;
      const bool not_first = bool(self.parser_ || !not_ssl());
      BOOST_ASIO_CORO_REENTER(*this)
      {
        // still need if statement, otherwise YIELD exits.
        if (!not_ssl())
        {
          MDEBUG("Performing SSL handshake to " << self_.get());
          connection<Sock>::set_timeout(self_, rest_handshake_timeout, false);
          BOOST_ASIO_CORO_YIELD async_handshake(self.sock_);
        }

        for (;;)
        {
          self.parser_.emplace();
          self.parser_->body_limit(max_endpoint_size);

          if (!connection<Sock>::set_timeout(self_, rest_request_timeout, not_first))
            return self.shutdown();

          MDEBUG("Reading new REST request from " << self_.get());
          BOOST_ASIO_CORO_YIELD boost::beast::http::async_read(
            self.sock_, self.buffer_, *self.parser_, self.strand_.wrap(std::move(*this))
          );

          // async_response will have its own timeouts set in handlers if async
          if (!self.timer_.cancel(error))
            return self.shutdown();

          /* async_response flow has MDEBUG statements for outgoing messages.
           async_response will also `self_->strand_.wrap` when necessary. */
          BOOST_ASIO_CORO_YIELD async_response(handler_loop{*this});

          connection<Sock>::set_timeout(self_, rest_response_timeout, false);
          BOOST_ASIO_CORO_YIELD boost::beast::http::async_write(
            self.sock_, self.response_, self.strand_.wrap(std::move(*this))
          );

          if (!self.keep_alive_)
            return self.shutdown();
        }
      }
    }
  };

  template<typename Sock>
  struct rest_server::accept_loop final : public boost::asio::coroutine
  {
    internal* self_;
    std::shared_ptr<connection<Sock>> next_;

    explicit accept_loop(internal* self) noexcept
      : self_(self), next_(nullptr)
    {}

    void operator()(boost::system::error_code error = {})
    {
      if (!self_)
        return;

      BOOST_ASIO_CORO_REENTER(*this)
      {
        for (;;)
        {
          next_ = std::make_shared<connection<Sock>>(self_);
          BOOST_ASIO_CORO_YIELD self_->acceptor.async_accept(next_->sock(), std::move(*this));

          if (error)
          {
            MERROR("Acceptor failed: " << error.message());
          }
          else
          {
            MDEBUG("New connection to " << next_->sock().remote_endpoint(error) << " / " << next_.get());
            next_->strand_.dispatch(handler_loop{next_});
          }
        }
      }
    }
  };

  void rest_server::run_io()
  {
    try { io_service_.run(); }
    catch (const std::exception& e)
    {
      std::raise(SIGINT);
      MERROR("Error in REST I/O thread: " << e.what());
    }
    catch (...)
    {
      std::raise(SIGINT);
      MERROR("Unexpected error in REST I/O thread");
    }
  }

  rest_server::rest_server(epee::span<const std::string> addresses, std::vector<std::string> admin, db::storage disk, rpc::client client, configuration config)
    : io_service_(), ports_(), workers_()
  {
    if (addresses.empty())
      MONERO_THROW(common_error::kInvalidArgument, "REST server requires 1 or more addresses");

    std::sort(admin.begin(), admin.end());
    const auto init_port = [&admin] (internal& port, const std::string& address, configuration config, const bool is_admin) -> bool
    {
      epee::net_utils::http::url_content url{};
      if (!epee::net_utils::parse_url(address, url))
        MONERO_THROW(lws::error::configuration, "REST server URL/address is invalid");

      const bool https = url.schema == "https";
      if (!https && url.schema != "http")
        MONERO_THROW(lws::error::configuration, "Unsupported scheme, only http or https supported");

      if (std::numeric_limits<std::uint16_t>::max() < url.port)
        MONERO_THROW(lws::error::configuration, "Specified port for REST server is out of range");

      if (!url.uri.empty() && url.uri.front() != '/')
        MONERO_THROW(lws::error::configuration, "First path prefix character must be '/'");

      if (!https)
      {
        boost::system::error_code error{};
        const boost::asio::ip::address ip_host =
          ip_host.from_string(url.host, error);
        if (error)
          MONERO_THROW(lws::error::configuration, "Invalid IP address for REST server");
        if (!ip_host.is_loopback() && !config.allow_external)
          MONERO_THROW(lws::error::configuration, "Binding to external interface with http - consider using https or secure tunnel (ssh, etc). Use --confirm-external-bind to override");
      }

      if (url.port == 0)
        url.port = https ? 8443 : 8080;

      if (!is_admin)
      {
        epee::net_utils::http::url_content admin_url{};
        const boost::string_ref start{address.c_str(), address.rfind(url.uri)};
        while (true) // try to merge 1+ admin prefixes
        {
          const auto mergeable = std::lower_bound(admin.begin(), admin.end(), start);
          if (mergeable == admin.end())
            break;

          if (!epee::net_utils::parse_url(*mergeable, admin_url))
            MONERO_THROW(lws::error::configuration, "Admin REST URL/address is invalid");
          if (admin_url.port == 0)
            admin_url.port = https ? 8443 : 8080;
          if (url.host != admin_url.host || url.port != admin_url.port)
            break; // nothing is mergeable

          if (port.admin_prefix)
            MONERO_THROW(lws::error::configuration, "Two admin REST servers cannot be merged onto one REST server");

          if (url.uri.size() < 2 || admin_url.uri.size() < 2)
            MONERO_THROW(lws::error::configuration, "Cannot merge REST server and admin REST server - a prefix must be specified for both");
          if (admin_url.uri.front() != '/')
            MONERO_THROW(lws::error::configuration, "Admin REST first path prefix character must be '/'");
          if (admin_url.uri != admin_url.m_uri_content.m_path)
            MONERO_THROW(lws::error::configuration, "Admin REST server must have path only prefix");

          MINFO("Merging admin and non-admin REST servers: " << address << " + " << *mergeable);
          port.admin_prefix = admin_url.m_uri_content.m_path;
          admin.erase(mergeable);
        } // while multiple mergable admins
      }

      if (url.uri != url.m_uri_content.m_path)
        MONERO_THROW(lws::error::configuration, "REST server must have path only prefix");

      if (url.uri.size() < 2)
        url.m_uri_content.m_path.clear();
      if (is_admin)
        port.admin_prefix = url.m_uri_content.m_path;
      else
        port.prefix = url.m_uri_content.m_path;

      epee::net_utils::ssl_options_t ssl_options = https ?
        epee::net_utils::ssl_support_t::e_ssl_support_enabled :
        epee::net_utils::ssl_support_t::e_ssl_support_disabled;
      ssl_options.verification = epee::net_utils::ssl_verification_t::none; // clients verified with view key
      ssl_options.auth = std::move(config.auth);

      boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::address::from_string(url.host),
        boost::lexical_cast<unsigned short>(url.port)
      };

      port.acceptor.open(endpoint.protocol());

#if !defined(_WIN32)
      port.acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
#endif

      port.acceptor.bind(endpoint);
      port.acceptor.listen();

      if (ssl_options)
      {
        port.ssl_ = ssl_options.create_context();
        accept_loop<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>{std::addressof(port)}();
      }
      else
        accept_loop<boost::asio::ip::tcp::socket>{std::addressof(port)}();
      return https;
    };

    bool any_ssl = false;
    const runtime_options options{config.max_subaddresses, config.webhook_verify, config.disable_admin_auth, config.auto_accept_creation};
    for (const std::string& address : addresses)
    {
      ports_.emplace_back(io_service_, disk.clone(), MONERO_UNWRAP(client.clone()), options);
      any_ssl |= init_port(ports_.back(), address, config, false);
    }

    for (const std::string& address : admin)
    {
      ports_.emplace_back(io_service_, disk.clone(), MONERO_UNWRAP(client.clone()), options);
      any_ssl |= init_port(ports_.back(), address, config, true);
    }

    const bool expect_ssl = !config.auth.private_key_path.empty();
    const std::size_t threads = config.threads;
    if (!any_ssl && expect_ssl)
      MONERO_THROW(lws::error::configuration, "Specified SSL key/cert without specifying https capable REST server");

    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
      workers_.emplace_back(std::bind(&rest_server::run_io, this));
  }

  rest_server::~rest_server() noexcept
  {
    io_service_.stop();
    for (auto& t : workers_)
    {
      if (t.joinable())
        t.join();
    }
  }
} // lws
