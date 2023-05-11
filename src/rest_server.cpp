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
#include <boost/utility/string_ref.hpp>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "common/error.h"                             // monero/src
#include "common/expect.h"                            // monero/src
#include "crypto/crypto.h"                            // monero/src
#include "cryptonote_config.h"      // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "error.h"
#include "lmdb/util.h"                 // monero/src
#include "net/http_base.h"             // monero/contrib/epee/include
#include "net/net_parse_helpers.h"     // monero/contrib/epee/include
#include "rpc/admin.h"
#include "rpc/client.h"
#include "rpc/daemon_messages.h"       // monero/src
#include "rpc/light_wallet.h"
#include "rpc/rates.h"
#include "util/http_server.h"
#include "util/gamma_picker.h"
#include "util/random_outputs.h"
#include "util/source_location.h"
#include "wire/crypto.h"
#include "wire/json.h"

namespace lws
{
  namespace
  {
    namespace http = epee::net_utils::http;

    struct context : epee::net_utils::connection_context_base
    {
      context()
        : epee::net_utils::connection_context_base()
      {}
    };

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

    namespace
    {
      std::atomic_flag rates_error_once = ATOMIC_FLAG_INIT;
    }

    struct get_address_info
    {
      using request = rpc::account_credentials;
      using response = rpc::get_address_info_response;

      static expect<response> handle(const request& req, db::storage disk, rpc::client const& client)
      {
        auto user = open_account(req, std::move(disk));
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

        resp.rates = client.get_rates();
        if (!resp.rates && !rates_error_once.test_and_set(std::memory_order_relaxed))
          MWARNING("Unable to retrieve exchange rates: " << resp.rates.error().message());

        return resp;
      }
    };

    struct get_address_txs
    {
      using request = rpc::account_credentials;
      using response = rpc::get_address_txs_response;

      static expect<response> handle(const request& req, db::storage disk, rpc::client const&)
      {
        auto user = open_account(req, std::move(disk));
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
      using response = rpc::get_random_outs_response;

      static expect<response> handle(request req, const db::storage&, rpc::client const& gclient)
      {
        using distribution_rpc = cryptonote::rpc::GetOutputDistribution;
        using histogram_rpc = cryptonote::rpc::GetOutputHistogram;
        using distribution_rpc = cryptonote::rpc::GetOutputDistribution;

        std::vector<std::uint64_t> amounts = std::move(req.amounts.values);

        if (50 < req.count || 20 < amounts.size())
          return {lws::error::exceeded_rest_request_limit};

        expect<rpc::client> client = gclient.clone();
        if (!client)
          return client.error();

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
          MONERO_CHECK(client->send(std::move(msg), std::chrono::seconds{10}));

          auto histogram_resp = client->receive<histogram_rpc::Response>(std::chrono::minutes{3}, MLWS_CURRENT_LOCATION);
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
          MONERO_CHECK(client->send(std::move(msg), std::chrono::seconds{10}));

          auto distribution_resp =
            client->receive<distribution_rpc::Response>(std::chrono::minutes{3}, MLWS_CURRENT_LOCATION);
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
          rpc::client gclient;
        public:
          zmq_fetch_keys(rpc::client src) noexcept
            : gclient(std::move(src))
          {}

          zmq_fetch_keys(zmq_fetch_keys&&) = default;
          zmq_fetch_keys(zmq_fetch_keys const& rhs)
            : gclient(MONERO_UNWRAP(rhs.gclient.clone()))
          {}

          expect<std::vector<output_keys>> operator()(std::vector<lws::output_ref> ids) const
          {
            using get_keys_rpc = cryptonote::rpc::GetOutputKeys;

            get_keys_rpc::Request keys_req{};
            keys_req.outputs = std::move(ids);

            expect<rpc::client> client = gclient.clone();
            if (!client)
              return client.error();

            epee::byte_slice msg = rpc::client::make_message("get_output_keys", keys_req);
            MONERO_CHECK(client->send(std::move(msg), std::chrono::seconds{10}));

            auto keys_resp = client->receive<get_keys_rpc::Response>(std::chrono::seconds{10}, MLWS_CURRENT_LOCATION);
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
          zmq_fetch_keys{std::move(*client)}
        );
        if (!rings)
          return rings.error();

        return response{std::move(*rings)};
      }
    };

    struct get_unspent_outs
    {
      using request = rpc::get_unspent_outs_request;
      using response = rpc::get_unspent_outs_response;

      static expect<response> handle(request req, db::storage disk, rpc::client const& gclient)
      {
        using rpc_command = cryptonote::rpc::GetFeeEstimate;

        auto user = open_account(req.creds, std::move(disk));
        if (!user)
          return user.error();

        expect<rpc::client> client = gclient.clone();
        if (!client)
          return client.error();

        {
          rpc_command::Request req{};
          req.num_grace_blocks = 10;
          epee::byte_slice msg = rpc::client::make_message("get_dynamic_fee_estimate", req);
          MONERO_CHECK(client->send(std::move(msg), std::chrono::seconds{10}));
        }

        if ((req.use_dust && req.use_dust) || !req.dust_threshold)
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

        const auto resp = client->receive<rpc_command::Response>(std::chrono::seconds{20}, MLWS_CURRENT_LOCATION);
        if (!resp)
          return resp.error();

        if (resp->size_scale == 0 || 1024 < resp->size_scale || resp->fee_mask == 0)
          return {lws::error::bad_daemon_response};

        const std::uint64_t per_byte_fee =
          resp->estimated_base_fee / resp->size_scale;

        return response{per_byte_fee, resp->fee_mask, rpc::safe_uint64(received), std::move(unspent), std::move(req.creds.key)};
      }
    };

    struct import_request
    {
      using request = rpc::account_credentials;
      using response = rpc::import_response;

      static expect<response> handle(request req, db::storage disk, rpc::client const&)
      {
        bool new_request = false;
        bool fulfilled = false;
        {
          auto user = open_account(req, disk.clone());
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
          MONERO_CHECK(disk.import_request(req.address, db::block_id(0)));

        const char* status = new_request ?
          "Accepted, waiting for approval" : (fulfilled ? "Approved" : "Waiting for Approval");
        return response{rpc::safe_uint64(0), status, new_request, fulfilled};
      }
    };

    struct login
    {
      using request = rpc::login_request;
      using response = rpc::login_response;

      static expect<response> handle(request req, db::storage disk, rpc::client const&)
      {
        if (!key_check(req.creds))
          return {lws::error::bad_view_key};

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
        MONERO_CHECK(disk.creation_request(req.creds.address, req.creds.key, flags));
        return response{true, req.generated_locally};
      }
    };

    struct submit_raw_tx
    {
      using request = rpc::submit_raw_tx_request;
      using response = rpc::submit_raw_tx_response;

      static expect<response> handle(request req, const db::storage& disk, const rpc::client& gclient)
      {
        using transaction_rpc = cryptonote::rpc::SendRawTxHex;

        expect<rpc::client> client = gclient.clone();
        if (!client)
          return client.error();

        transaction_rpc::Request daemon_req{};
        daemon_req.relay = true;
        daemon_req.tx_as_hex = std::move(req.tx);

        epee::byte_slice message = rpc::client::make_message("send_raw_tx_hex", daemon_req);
        MONERO_CHECK(client->send(std::move(message), std::chrono::seconds{10}));

        const auto daemon_resp = client->receive<transaction_rpc::Response>(std::chrono::seconds{20}, MLWS_CURRENT_LOCATION);
        if (!daemon_resp)
          return daemon_resp.error();
        if (!daemon_resp->relayed)
          return {lws::error::tx_relay_failed};

        return response{"OK"};
      }
    };

    template<typename E>
    expect<epee::byte_slice> call(std::string&& root, db::storage disk, const rpc::client& gclient, const bool)
    {
      using request = typename E::request;
      using response = typename E::response;

      expect<request> req = wire::json::from_bytes<request>(std::move(root));
      if (!req)
        return req.error();

      expect<response> resp = E::handle(std::move(*req), std::move(disk), gclient);
      if (!resp)
        return resp.error();
      return wire::json::to_bytes<response>(*resp);
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
    expect<epee::byte_slice> call_admin(std::string&& root, db::storage disk, const rpc::client&, const bool disable_auth)
    {
      using request = typename E::request;
      expect<admin<request>> req = wire::json::from_bytes<admin<request>>(std::move(root));
      if (!req)
        return req.error();

      if (!disable_auth)
      {
        if (!req->auth)
          return {error::account_not_found};

        db::account_address address{};
        if (!crypto::secret_key_to_public_key(*(req->auth), address.view_public))
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
      MONERO_CHECK(E{}(dest, std::move(disk), std::move(req->params)));
      return dest.take_bytes();
    }

    struct endpoint
    {
      char const* const name;
      expect<epee::byte_slice> (*const run)(std::string&&, db::storage, rpc::client const&, bool);
      const unsigned max_size;
    };

    constexpr const endpoint endpoints[] =
    {
      {"/get_address_info",      call<get_address_info>, 2 * 1024},
      {"/get_address_txs",       call<get_address_txs>,  2 * 1024},
      {"/get_random_outs",       call<get_random_outs>,  2 * 1024},
      {"/get_txt_records",       nullptr,                0       },
      {"/get_unspent_outs",      call<get_unspent_outs>, 2 * 1024},
      {"/import_wallet_request", call<import_request>,   2 * 1024},
      {"/login",                 call<login>,            2 * 1024},
      {"/submit_raw_tx",         call<submit_raw_tx>,   50 * 1024}
    };

    constexpr const endpoint admin_endpoints[] =
    {
      {"/accept_requests",       call_admin<rpc::accept_requests_>, 50 * 1024},
      {"/add_account",           call_admin<rpc::add_account_>,     50 * 1024},
      {"/list_accounts",         call_admin<rpc::list_accounts_>,   100},
      {"/list_requests",         call_admin<rpc::list_requests_>,   100},
      {"/modify_account_status", call_admin<rpc::modify_account_>,  50 * 1024},
      {"/reject_requests",       call_admin<rpc::reject_requests_>, 50 * 1024},
      {"/rescan",                call_admin<rpc::rescan_>,          50 * 1024},
      {"/webhook_add",           call_admin<rpc::webhook_add_>,     50 * 1024},
      {"/webhook_delete",        call_admin<rpc::webhook_delete_>,  50 * 1024},
      {"/webhook_delete_uuid",   call_admin<rpc::webhook_del_uuid_>,50 * 1024},
      {"/webhook_list",          call_admin<rpc::webhook_list_>,    100}
    };

    struct by_name_
    {
      bool operator()(endpoint const& left, endpoint const& right) const noexcept
      {
        if (left.name && right.name)
          return std::strcmp(left.name, right.name) < 0;
        return false;
      }
      bool operator()(const boost::string_ref left, endpoint const& right) const noexcept
      {
        if (right.name)
          return left < right.name;
        return false;
      }
      bool operator()(endpoint const& left, const boost::string_ref right) const noexcept
      {
        if (left.name)
          return left.name < right;
        return false;
      }
    };
    constexpr const by_name_ by_name{};
  } // anonymous

  struct rest_server::internal final : public lws::http_server_impl_base<rest_server::internal, context>
  {
    db::storage disk;
    rpc::client client;
    boost::optional<std::string> prefix;
    boost::optional<std::string> admin_prefix;
    bool disable_auth;

    explicit internal(boost::asio::io_service& io_service, lws::db::storage disk, rpc::client client, const bool disable_auth)
      : lws::http_server_impl_base<rest_server::internal, context>(io_service)
      , disk(std::move(disk))
      , client(std::move(client))
      , prefix()
      , admin_prefix()
      , disable_auth(disable_auth)
    {
      assert(std::is_sorted(std::begin(endpoints), std::end(endpoints), by_name));
    }

    const endpoint* get_endpoint(boost::string_ref uri) const
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

    virtual bool
    handle_http_request(const http::http_request_info& query, http::http_response_info& response, context& ctx)
    override final
    {
      endpoint const* const handler = get_endpoint(query.m_URI);
      if (!handler)
      {
        response.m_response_code = 404;
        response.m_response_comment = "Not Found";
        return true;
      }

      if (handler->run == nullptr)
      {
        response.m_response_code = 501;
        response.m_response_comment = "Not Implemented";
        return true;
      }

      if (handler->max_size < query.m_body.size())
      {
        MINFO("Client exceeded maximum body size (" << handler->max_size << " bytes)");
        response.m_response_code = 400;
        response.m_response_comment = "Bad Request";
        return true;
      }

      if (query.m_http_method != http::http_method_post)
      {
        response.m_response_code = 405;
        response.m_response_comment = "Method Not Allowed";
        return true;
      }

      // \TODO remove copy of json string here :/
      auto body = handler->run(std::string{query.m_body}, disk.clone(), client, disable_auth);
      if (!body)
      {
        MINFO(body.error().message() << " from " << ctx.m_remote_address.str() << " on " << handler->name);

        if (body.error().category() == wire::error::rapidjson_category())
        {
          response.m_response_code = 400;
          response.m_response_comment = "Bad Request";
        }
        else if (body == lws::error::account_not_found || body == lws::error::duplicate_request)
        {
          response.m_response_code = 403;
          response.m_response_comment = "Forbidden";
        }
        else if (body.matches(std::errc::timed_out) || body.matches(std::errc::no_lock_available))
        {
          response.m_response_code = 503;
          response.m_response_comment = "Service Unavailable";
        }
        else
        {
          response.m_response_code = 500;
          response.m_response_comment = "Internal Server Error";
        }
        return true;
      }

      response.m_response_code = 200;
      response.m_response_comment = "OK";
      response.m_mime_tipe = "application/json";
      response.m_header_info.m_content_type = "application/json";
      response.m_body.assign(reinterpret_cast<const char*>(body->data()), body->size()); // \TODO Remove copy here too!s
      return true;
    }
  };

  rest_server::rest_server(epee::span<const std::string> addresses, std::vector<std::string> admin, db::storage disk, rpc::client client, configuration config)
    : io_service_(), ports_()
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

      if (!port.init(std::to_string(url.port), std::move(url.host), std::move(config.access_controls), std::move(ssl_options)))
        MONERO_THROW(lws::error::http_server, "REST server failed to initialize");
      return https;
    };

    bool any_ssl = false;
    for (const std::string& address : addresses)
    {
      ports_.emplace_back(io_service_, disk.clone(), MONERO_UNWRAP(client.clone()), config.disable_admin_auth);
      any_ssl |= init_port(ports_.back(), address, config, false);
    }

    for (const std::string& address : admin)
    {
      ports_.emplace_back(io_service_, disk.clone(), MONERO_UNWRAP(client.clone()), config.disable_admin_auth);
      any_ssl |= init_port(ports_.back(), address, config, true);
    }

    const bool expect_ssl = !config.auth.private_key_path.empty();
    const std::size_t threads = config.threads;
    if (!any_ssl && expect_ssl)
      MONERO_THROW(lws::error::configuration, "Specified SSL key/cert without specifying https capable REST server");

    if (!ports_.front().run(threads, false))
      MONERO_THROW(lws::error::http_server, "REST server failed to run");
  }

  rest_server::~rest_server() noexcept
  {}
} // lws
