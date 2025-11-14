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
#include "scanner.h"

#include <algorithm>
#include <boost/asio/use_future.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/range/combine.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/error.h"                             // monero/src
#include "config.h"
#include "crypto/crypto.h"                            // monero/src
#include "crypto/wallet/crypto.h"                     // monero/src
#include "cryptonote_basic/cryptonote_basic.h"        // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "db/account.h"
#include "db/data.h"
#include "cryptonote_basic/difficulty.h"              // monero/src
#include "error.h"
#include "hardforks/hardforks.h"   // monero/src
#include "misc_log_ex.h"           // monero/contrib/epee/include
#include "net/net_parse_helpers.h"
#include "net/net_ssl.h"           // monero/contrib/epee/include
#include "net/net_utils_base.h"    // monero/contrib/epee/include
#include "rpc/daemon_messages.h"   // monero/src
#include "rpc/daemon_zmq.h"
#include "rpc/json.h"
#include "rpc/lws_pub.h"
#include "rpc/message_data_structs.h" // monero/src
#include "rpc/scanner/queue.h"
#include "rpc/scanner/server.h"
#include "rpc/webhook.h"
#include "util/blocks.h"
#include "util/source_location.h"
#include "util/transactions.h"

#include "serialization/json_object.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"
#define MINIMUM_BLOCK_DEPTH 16

namespace lws
{
  namespace
  {
    namespace enet = epee::net_utils;

    constexpr const std::chrono::minutes block_rpc_timeout{2};
    constexpr const std::chrono::seconds send_timeout{30};
    constexpr const std::chrono::seconds sync_rpc_timeout{30};

    struct thread_data
    {
      explicit thread_data(rpc::client client, db::storage disk, std::vector<lws::account> users, std::shared_ptr<rpc::scanner::queue> queue, scanner_options opts)
        : client(std::move(client)), disk(std::move(disk)), users(std::move(users)), queue(std::move(queue)), opts(std::move(opts))
      {}

      rpc::client client;
      db::storage disk;
      std::vector<lws::account> users;
      std::shared_ptr<rpc::scanner::queue> queue;
      scanner_options opts;
    };
 
    bool is_new_block(std::string&& chain_msg, std::optional<db::storage>& disk, const account& user)
    {
      const auto chain = rpc::minimal_chain_pub::from_json(std::move(chain_msg));
      if (!chain)
      {
        MERROR("Unable to parse blockchain notification: " << chain.error());
        return false;
      }

      if (user.scan_height() < db::block_id(chain->top_block_height))
        return true;

      if (!disk)
      {
        MWARNING("Assuming new block - no access to local DB");
        return true;
      }

      auto reader = disk->start_read();
      if (!reader)
      {
        MWARNING("Failed to start DB read: " << reader.error());
        return true;
      }

      // check possible chain rollback daemon side
      const expect<crypto::hash> id = reader->get_block_hash(db::block_id(chain->top_block_height));
      if (!id || *id != chain->top_block_id)
        return true;

      // check possible chain rollback from other thread
      const expect<db::account> user_db = reader->get_account(db::account_status::active, user.id());
      return !user_db || user_db->scan_height != user.scan_height();
    }

    bool send(rpc::client& client, epee::byte_slice message)
    {
      const expect<void> sent = client.send(std::move(message), send_timeout);
      if (!sent)
      {
        if (sent.matches(std::errc::interrupted))
          return false;
        MONERO_THROW(sent.error(), "Failed to send ZMQ RPC message");
      }
      return true;
    }

    void send_payment_hook(boost::asio::io_context& io, rpc::client& client, net::http::client& http, const epee::span<const db::webhook_tx_confirmation> events)
    {
      rpc::send_webhook_async(io, client, http, events, "json-full-payment_hook:", "msgpack-full-payment_hook:");
    }
 
    std::size_t get_target_time(db::block_id height)
    {
      const hardfork_t* fork = nullptr;
      switch (config::network)
      {
      case cryptonote::network_type::MAINNET:
        if (num_mainnet_hard_forks < 2)
          MONERO_THROW(error::bad_blockchain, "expected more mainnet forks");
        fork = mainnet_hard_forks;
        break;     
      case cryptonote::network_type::TESTNET:
         if (num_testnet_hard_forks < 2)
          MONERO_THROW(error::bad_blockchain, "expected more testnet forks");
        fork = testnet_hard_forks;
        break; 
      case cryptonote::network_type::STAGENET:
         if (num_stagenet_hard_forks < 2)
          MONERO_THROW(error::bad_blockchain, "expected more stagenet forks");
        fork = stagenet_hard_forks;
        break;
      default:
        MONERO_THROW(error::bad_blockchain, "chain type not support with full sync"); 
      } 
      // this is hardfork version 2
      return height < db::block_id(fork[1].height) ?
        DIFFICULTY_TARGET_V1 : DIFFICULTY_TARGET_V2;
    }

    //! For difficulty vectors only
    template<typename T> 
    void update_window(T& vec)
    {
      // should only have one to pop each time
      while (DIFFICULTY_BLOCKS_COUNT < vec.size())
        vec.erase(vec.begin());
    };

    void send_spend_hook(boost::asio::io_context& io, rpc::client& client, net::http::client& http, const epee::span<const db::webhook_tx_spend> events)
    {
      rpc::send_webhook_async(io, client, http, events, "json-full-spend_hook:", "msgpack-full-spend_hook:");
    }
 
    struct add_spend
    {
      void operator()(lws::account& user, const db::spend& spend) const
      { user.add_spend(spend); }
    };
    struct add_output
    {
      bool operator()(expect<db::storage_reader>&, lws::account& user, const db::output& out) const
      { return user.add_out(out); }
    };

    struct null_spend
    {
      void operator()(lws::account&, const db::spend&) const noexcept
      {}
    };
    struct send_webhook
    {
      db::storage const& disk_;
      rpc::client& client_;
      scanner_sync& http_;
      std::unordered_map<crypto::hash, crypto::hash> txpool_;

      bool operator()(expect<db::storage_reader>& reader, lws::account& user, const db::output& out)
      {
        /* Upstream monerod does not send all fields for a transaction, so
           mempool notifications cannot compute tx_hash correctly (it is not
           sent separately, a further blunder). Instead, if there are matching
           outputs with webhooks, fetch mempool to compare tx_prefix_hash and
           then use corresponding tx_hash. */
        const db::webhook_key key{user.id(), db::webhook_type::tx_confirmation};
        std::vector<db::webhook_value> hooks{};

        {
          db::storage_reader* active_reader = reader ?
            std::addressof(*reader) : nullptr;

          expect<db::storage_reader> temp_reader{common_error::kInvalidArgument};
          if (!active_reader)
          {
            temp_reader = disk_.start_read();
            if (!temp_reader)
            {
              MERROR("Unable to lookup webhook on tx in pool: " << reader.error().message());
              return false;
            }
            active_reader = std::addressof(*temp_reader);
          }
          auto found = active_reader->find_webhook(key, out.payment_id.short_);
          if (!found)
          {
            MERROR("Failed db lookup for webhooks: " << found.error().message());
            return false;
          }
          hooks = std::move(*found);
        }

        if (!hooks.empty() && txpool_.empty())
        {
          cryptonote::rpc::GetTransactionPool::Request req{};
          if (!send(client_, rpc::client::make_message("get_transaction_pool", req)))
          {
            MERROR("Unable to compute tx hash for webhook, aborting");
            return false;
          }
          auto resp = client_.get_message(std::chrono::seconds{3});
          if (!resp)
          {
            MERROR("Unable to get txpool: " << resp.error().message());
            return false;
          }

          auto txpool = rpc::parse_json_response<rpc::get_transaction_pool>(std::move(*resp));
          if (!txpool)
            MONERO_THROW(txpool.error(), "Failed fetching transaction pool");
          for (auto& tx : txpool->transactions)
            txpool_.emplace(get_transaction_prefix_hash(tx.tx), tx.tx_hash);
        }

        std::vector<db::webhook_tx_confirmation> events{};
        for (auto& hook : hooks)
        {
          events.push_back(db::webhook_tx_confirmation{key, std::move(hook), out});
          events.back().value.second.confirmations = 0;

          const auto hash = txpool_.find(out.tx_prefix_hash);
          if (hash != txpool_.end())
            events.back().tx_info.link.tx_hash = hash->second;
          else
            events.pop_back(); //cannot compute tx_hash
        }
        send_payment_hook(http_.io_, client_, http_.webhooks_, epee::to_span(events));
        return true;
      }
    };

    struct subaddress_reader
    {
      expect<db::storage_reader> reader;
      std::optional<db::storage> disk;
      db::cursor::subaddress_indexes cur;
      const std::uint32_t max_subaddresses;

      subaddress_reader(std::optional<db::storage> const& disk_in, const std::uint32_t max_subaddresses)
        : reader(common_error::kInvalidArgument), disk(), cur(nullptr), max_subaddresses(max_subaddresses)
      {
        if (disk_in)
          disk = disk_in->clone();

        if (max_subaddresses)
          update_reader();
      }

      void update_reader()
      {
        if (disk)
          reader = disk->start_read();
        if (!reader)
          MERROR("Subadress lookup failure: " << reader.error().message());
      }
    };

    void update_lookahead(const account& user, subaddress_reader& reader, const db::address_index& match, const db::block_id height)
    {
      if (match.is_zero())
        return; // keep subaddress disabled servers quick

      if (!reader.disk)
        throw std::runtime_error{"Bad DB handle in scanner"};

      auto upserted = reader.disk->update_lookahead(user.db_address(), height, match, reader.max_subaddresses);
      if (upserted)
      {
        if (0 < *upserted)
          reader.update_reader(); // update reader after upsert added new addresses
        else if (*upserted < 0)
          upserted = {error::max_subaddresses};
      }

      if (!upserted)
        MWARNING("Failed to update lookahead for " << user.address() << ": " << upserted.error());
    }

    void scan_transaction_base(
      epee::span<lws::account> users,
      const db::block_id height,
      const std::uint64_t timestamp,
      crypto::hash const& tx_hash,
      cryptonote::transaction const& tx,
      std::vector<std::uint64_t> const& out_ids,
      subaddress_reader& reader,
      std::function<void(lws::account&, const db::spend&)> spend_action,
      std::function<bool(expect<db::storage_reader>&, lws::account&, const db::output&)> output_action)
    {
      if (2 < tx.version)
        throw std::runtime_error{"Unsupported tx version"};

      cryptonote::tx_extra_pub_key key;
      boost::optional<crypto::hash> prefix_hash;
      boost::optional<cryptonote::tx_extra_nonce> extra_nonce;
      std::pair<std::uint8_t, db::output::payment_id_> payment_id;
      cryptonote::tx_extra_additional_pub_keys additional_tx_pub_keys;
      std::vector<crypto::key_derivation> additional_derivations;

      {
        std::vector<cryptonote::tx_extra_field> extra;
        cryptonote::parse_tx_extra(tx.extra, extra);
        // allow partial parsing of tx extra (similar to wallet2.cpp)

        if (!cryptonote::find_tx_extra_field_by_type(extra, key))
          return;

        extra_nonce.emplace();
        if (cryptonote::find_tx_extra_field_by_type(extra, *extra_nonce))
        {
          if (cryptonote::get_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.long_))
            payment_id.first = sizeof(crypto::hash);
        }
        else
          extra_nonce = boost::none;

        // additional tx pub keys present when there are 3+ outputs in a tx involving subaddresses
        if (reader.reader)
          cryptonote::find_tx_extra_field_by_type(extra, additional_tx_pub_keys);
      } // destruct `extra` vector

      for (account& user : users)
      {
        if (height <= user.scan_height())
          continue; // to next user

        crypto::key_derivation derived;
        if (!crypto::wallet::generate_key_derivation(key.pub_key, user.view_key(), derived))
          continue; // to next user

        if (reader.reader && additional_tx_pub_keys.data.size() == tx.vout.size())
        {
          additional_derivations.resize(tx.vout.size());
          std::size_t index = -1;
          for (auto const& out: tx.vout)
          {
            ++index;
            if (!crypto::wallet::generate_key_derivation(additional_tx_pub_keys.data[index], user.view_key(), additional_derivations[index]))
            {
              additional_derivations.clear();
              break; // vout loop
            }
          }
        }

        db::extra ext{};
        std::uint32_t mixin = 0;
        for (auto const& in : tx.vin)
        {
          cryptonote::txin_to_key const* const in_data =
            boost::get<cryptonote::txin_to_key>(std::addressof(in));
          if (in_data)
          {
            mixin = boost::numeric_cast<std::uint32_t>(
              std::max(std::size_t(1), in_data->key_offsets.size()) - 1
            );

            std::uint64_t goffset = 0;
            for (std::uint64_t offset : in_data->key_offsets)
            {
              goffset += offset;
              const boost::optional<db::address_index> subaccount =
                user.get_spendable(db::output_id{in_data->amount, goffset});
              if (!subaccount)
                continue; // to next input

              spend_action(
                user,
                db::spend{
                  db::transaction_link{height, tx_hash},
                  in_data->k_image,
                  db::output_id{in_data->amount, goffset},
                  timestamp,
                  tx.unlock_time,
                  mixin,
                  {0, 0, 0}, // reserved
                  payment_id.first,
                  payment_id.second.long_,
                  *subaccount
                }
              );
            }
          }
          else if (boost::get<cryptonote::txin_gen>(std::addressof(in)))
            ext = db::extra(ext | db::coinbase_output);
        }

        std::size_t index = -1;
        for (auto const& out : tx.vout)
        {
          ++index;

          crypto::public_key out_pub_key;
          if (!cryptonote::get_output_public_key(out, out_pub_key))
            continue; // to next output

          boost::optional<crypto::view_tag> view_tag_opt =
            cryptonote::get_output_view_tag(out);

          const bool found_tag =
            (!additional_derivations.empty() && cryptonote::out_can_be_to_acc(view_tag_opt, additional_derivations.at(index), index)) ||
            cryptonote::out_can_be_to_acc(view_tag_opt, derived, index); 

          if (!found_tag)
            continue; // to next output

          bool found_pub = false;
          db::address_index account_index{db::major_index::primary, db::minor_index::primary};
          crypto::key_derivation active_derived{};
          crypto::public_key active_pub{};

          // inspect the additional and traditional keys
          for (std::size_t attempt = 0; attempt < 2; ++attempt)
          {
            if (attempt == 0)
            {
              active_derived = derived;
              active_pub = key.pub_key;
            }
            else if (!additional_derivations.empty())
            {
              active_derived = additional_derivations.at(index);
              active_pub = additional_tx_pub_keys.data.at(index);
            }
            else
              break; // inspection loop

            crypto::public_key derived_pub;
            if (!crypto::wallet::derive_subaddress_public_key(out_pub_key, active_derived, index, derived_pub))
              continue; // to next available active_derived

            if (user.spend_public() != derived_pub)
            {
              if (!reader.reader)
                continue; // to next available active_derived

              const expect<db::address_index> match =
                reader.reader->find_subaddress(user.id(), derived_pub, reader.cur);
              if (!match)
              {
                if (match != lmdb::error(MDB_NOTFOUND))
                  MERROR("Failure when doing subaddress search: " << match.error().message());
                continue; // to next available active_derived
              }

              update_lookahead(user, reader, *match, height);
              found_pub = true;
              account_index = *match;
              break; // additional_derivations loop
            }
            else
            {
              found_pub = true;
              break; // additional_derivations loop
            }
          }

          if (!found_pub)
            continue; // to next output

          if (!prefix_hash)
          {
            prefix_hash.emplace();
            cryptonote::get_transaction_prefix_hash(tx, *prefix_hash);
          }

          std::uint64_t amount = out.amount;
          rct::key mask = rct::identity();
          if (!amount && !(ext & db::coinbase_output) && 1 < tx.version)
          {
            const bool bulletproof2 = (rct::RCTTypeBulletproof2 <= tx.rct_signatures.type);
            const auto decrypted = lws::decode_amount(
              tx.rct_signatures.outPk.at(index).mask, tx.rct_signatures.ecdhInfo.at(index), active_derived, index, bulletproof2
            );
            if (!decrypted)
            {
              MWARNING(user.address() << " failed to decrypt amount for tx " << tx_hash << ", skipping output");
              continue; // to next output
            }
            amount = decrypted->first;
            mask = decrypted->second;
            ext = db::extra(ext | db::ringct_output);
          }
          else if (1 < tx.version)
            ext = db::extra(ext | db::ringct_output);

          if (extra_nonce)
          {
            if (!payment_id.first && cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.short_))
            {
              payment_id.first = sizeof(crypto::hash8);
              lws::decrypt_payment_id(payment_id.second.short_, active_derived);
            }
          }
          const bool added = output_action(
            reader.reader,
            user,
            db::output{
              db::transaction_link{height, tx_hash},
              db::output::spend_meta_{
                db::output_id{tx.version < 2 ? out.amount : 0, out_ids.at(index)},
                amount,
                mixin,
                boost::numeric_cast<std::uint32_t>(index),
                active_pub
              },
              timestamp,
              tx.unlock_time,
              *prefix_hash,
              out_pub_key,
              mask,
              {0, 0, 0, 0, 0, 0, 0}, // reserved bytes
              db::pack(ext, payment_id.first),
              payment_id.second,
              cryptonote::get_tx_fee(tx),
              account_index
            }
          );

          if (!added)
            MWARNING("Output not added, duplicate public key encountered");
        } // for all tx outs
      } // for all users
    }

    void scan_transaction(
      epee::span<lws::account> users,
      const db::block_id height,
      const std::uint64_t timestamp,
      crypto::hash const& tx_hash,
      cryptonote::transaction const& tx,
      std::vector<std::uint64_t> const& out_ids,
      subaddress_reader& reader)
    {
      scan_transaction_base(users, height, timestamp, tx_hash, tx, out_ids, reader, add_spend{}, add_output{});
    }

    void scan_transactions(std::string&& txpool_msg, epee::span<lws::account> users, db::storage const& disk, scanner_sync& self, rpc::client& client, const scanner_options& opts)
    {
      // uint64::max is for txpool
      static const std::vector<std::uint64_t> fake_outs(
        256, std::numeric_limits<std::uint64_t>::max()
      );

      const auto parsed = rpc::full_txpool_pub::from_json(std::move(txpool_msg));
      if (!parsed)
      {
        MERROR("Failed parsing txpool pub: " << parsed.error().message());
        return;
      }

      const auto time =
        boost::numeric_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

      subaddress_reader reader{std::optional<db::storage>{disk.clone()}, opts.max_subaddresses};
      send_webhook sender{disk, client, self};
      for (const auto& tx : parsed->txes)
        scan_transaction_base(users, db::block_id::txpool, time, crypto::hash{}, tx, fake_outs, reader, null_spend{}, sender);
    }

    void do_scan_loop(scanner_sync& self, std::shared_ptr<thread_data> data, const size_t thread_n) noexcept
    {
      struct stop_
      {
        scanner_sync& self;
        ~stop_() { self.stop(); }
      } stop{self};

      // thread entry point, so wrap everything in `try { } catch (...) {}`
      try
      { 
        // boost::thread doesn't support move-only types + attributes
        rpc::client client{std::move(data->client)};
        db::storage disk{std::move(data->disk)};
        std::vector<lws::account> users{std::move(data->users)};
        const std::shared_ptr<rpc::scanner::queue> queue{std::move(data->queue)};
        const scanner_options opts{std::move(data->opts)};
        
        data.reset();

        if (!queue)
          return;

        while (self.is_running())
        {
          if (!users.empty())
          {
            auto new_client = MONERO_UNWRAP(client.clone());
            MONERO_UNWRAP(new_client.watch_scan_signals());
            user_data store_local{disk.clone()};
            if (!scanner::loop(self, std::move(store_local), disk.clone(), std::move(new_client), std::move(users), *queue, opts, thread_n))
              return;
          }

          users.clear();
          MINFO("Thread " << thread_n << " has gone idle, waiting for work");
          auto status = queue->wait_for_accounts();
          if (status.replace)
          {
            users = std::move(*status.replace);
            MINFO("Thread " << thread_n << " received " << users.size() << " replacement account(s), starting work");
          }
          if (!status.push.empty())
          {
            MINFO("Thread " << thread_n << " received " << status.push.size() << " new account(s)");
            users.insert(
              users.end(),
              std::make_move_iterator(status.push.begin()),
              std::make_move_iterator(status.push.end())
            );
            if (!status.replace)
              MINFO("Thread " << thread_n << " now has " << users.size() << " total account(s), starting work");
          }
        }
      }
      catch (std::exception const& e)
      {
        self.shutdown();
        MERROR("Scanner shutdown with error " << e.what());
      }
      catch (...)
      {
        self.shutdown();
        MERROR("Unknown exception");
      }
    }
  } // anonymous

  scanner::scanner(db::storage disk, epee::net_utils::ssl_verification_t webhook_verify)
    : disk_(std::move(disk)), sync_(webhook_verify), signals_(sync_.io_)
  {
    signals_.add(SIGINT);
    signals_.async_wait([this] (const boost::system::error_code& error, int)
      {
        if (error != boost::asio::error::operation_aborted)
          shutdown(); 
      });
  }

  scanner::~scanner()
  {}

    bool scanner::loop(scanner_sync& self, store_func store, std::optional<db::storage> disk, rpc::client client, std::vector<lws::account> users, rpc::scanner::queue& queue, const scanner_options& opts, const size_t thread_n)
    {
      const bool leader_thread = thread_n == 0;

      if (users.empty())
        return true;

      { // previous `try` block; leave to prevent git blame spam
        std::sort(users.begin(), users.end(), by_height{});

        /// RPC server assumes that `start_height == 0` means use
        // block ids. This technically skips genesis block.
        cryptonote::rpc::GetBlocksFast::Request req{};
        req.start_height = std::uint64_t(users.begin()->scan_height());
        req.start_height = std::max(std::uint64_t(1), req.start_height);
        req.prune = !opts.untrusted_daemon;

        epee::byte_slice block_request = rpc::client::make_message("get_blocks_fast", req);
        if (!send(client, block_request.clone()))
          return false;

        std::vector<crypto::hash> blockchain{};
        std::vector<db::pow_sync> new_pow{};
        db::pow_window pow_window{};

        db::block_id last_pow{};
        if (opts.untrusted_daemon && disk)
          last_pow = MONERO_UNWRAP(MONERO_UNWRAP(disk->start_read()).get_last_pow_block()).id;

        while (!self.stop_)
        {
          blockchain.clear();
          new_pow.clear();

          auto resp = client.get_message(block_rpc_timeout);
          if (!resp)
          {
            const bool timeout = resp.matches(std::errc::timed_out);
            if (timeout)
              MWARNING("Block retrieval timeout, resetting scanner");
            if (timeout || resp.matches(std::errc::interrupted))
              return false;
            MONERO_THROW(resp.error(), "Failed to retrieve blocks from daemon");
          }

          auto fetched = rpc::parse_json_response<rpc::get_blocks_fast>(std::move(*resp));
          if (!fetched)
          {
            MERROR("Failed to retrieve next blocks: " << fetched.error().message() << ". Resetting state and trying again");
            return false;
          }

          if (fetched->blocks.empty())
            throw std::runtime_error{"Daemon unexpectedly returned zero blocks"};

          if (fetched->start_height != req.start_height)
          {
            MWARNING("Daemon sent wrong blocks, resetting state");
            return false;
          }

          {
            bool resort = false;
            auto status = queue.get_accounts();
            if (status.replace && status.replace->empty() && status.push.empty())
              return true; // no work explictly given, leave

            if (status.replace)
            {
              MINFO("Received " << status.replace->size() << " replacement account(s) for scanning");
              users = std::move(*status.replace);
              resort = true;
            }
            if (!status.push.empty())
            {
              MINFO("Received " << status.push.size() << " new account(s) for scanning");
              users.insert(
                users.end(),
                std::make_move_iterator(status.push.begin()),
                std::make_move_iterator(status.push.end())
              );
              resort = true;
            }
  
            if (resort)
            {
              assert(!users.empty()); // by logic from above
              std::sort(users.begin(), users.end(), by_height{});
              const db::block_id oldest = users.front().scan_height();
              if (std::uint64_t(oldest) < fetched->start_height)
              {
                req.start_height = std::uint64_t(oldest);
                block_request = rpc::client::make_message("get_blocks_fast", req);
                if (!send(client, block_request.clone()))
                  return false;
                continue; // to next get_blocks_fast read
              }
              // else, the oldest new account is within the newly fetch range
            }
          }

          // prep for next blocks retrieval
          req.start_height = fetched->start_height + fetched->blocks.size() - 1;
          block_request = rpc::client::make_message("get_blocks_fast", req);

          if (fetched->blocks.size() <= 1)
          {
            // synced to top of chain, wait for next blocks
            for (bool wait_for_block = true; wait_for_block; )
            {
              expect<std::vector<std::pair<rpc::client::topic, std::string>>> new_pubs = client.wait_for_block();
              if (new_pubs.matches(std::errc::interrupted))
                return false; // reset entire state (maybe shutdown)

              if (!new_pubs)
                break; // exit wait for block loop, and try fetching new blocks

              // put txpool messages before block messages
              static_assert(rpc::client::topic::block < rpc::client::topic::txpool, "bad sort");
              std::sort(new_pubs->begin(), new_pubs->end(), std::greater<>{});

              // process txpool first
              auto message = new_pubs->begin();
              for ( ; message != new_pubs->end(); ++message)
              {
                if (!disk || message->first != rpc::client::topic::txpool)
                  break; // inner for loop
                scan_transactions(std::move(message->second), epee::to_mut_span(users), *disk, self, client, opts);
              }

              for ( ; message != new_pubs->end(); ++message)
              {
                if (message->first == rpc::client::topic::block && is_new_block(std::move(message->second), disk, users.front()))
                  wait_for_block = false;
              }
            } // wait for block

            // request next chunk of blocks
            if (!send(client, block_request.clone()))
              return false;
            continue; // to next get_blocks_fast read
          } // if only one block was fetched

          // request next chunk of blocks
          if (!send(client, block_request.clone()))
            return false;

          if (fetched->blocks.size() != fetched->output_indices.size())
            throw std::runtime_error{"Bad daemon response - need same number of blocks and indices"};

          blockchain.push_back(cryptonote::get_block_hash(fetched->blocks.front().block));
          if (opts.untrusted_daemon)
            new_pow.push_back(db::pow_sync{fetched->blocks.front().block.timestamp});

          auto blocks = epee::to_mut_span(fetched->blocks);
          auto indices = epee::to_span(fetched->output_indices);

          if (fetched->start_height != 1)
          {
            // skip overlap block
            blocks.remove_prefix(1);
            indices.remove_prefix(1);
          }
          else
            fetched->start_height = 0;

          if (disk && opts.untrusted_daemon)
          {
            pow_window = MONERO_UNWRAP(
              MONERO_UNWRAP(disk->start_read()).get_pow_window(db::block_id(fetched->start_height))
            );
          }

          subaddress_reader reader{disk, opts.max_subaddresses};
          db::block_difficulty::unsigned_int diff{};
          const db::block_id initial_height = db::block_id(fetched->start_height);
          for (auto block_data : boost::combine(blocks, indices))
          {
            ++(fetched->start_height);

            cryptonote::block const& block = boost::get<0>(block_data).block;
            auto const& txes = boost::get<0>(block_data).transactions;

            if (block.tx_hashes.size() != txes.size())
              throw std::runtime_error{"Bad daemon response - need same number of txes and tx hashes"};

            auto indices = epee::to_span(boost::get<1>(block_data));
            if (indices.empty())
              throw std::runtime_error{"Bad daemon response - missing /coinbase tx indices"};

            crypto::hash miner_tx_hash;
            if (!cryptonote::get_transaction_hash(block.miner_tx, miner_tx_hash))
              throw std::runtime_error{"Failed to calculate miner tx hash"};

            scan_transaction(
              epee::to_mut_span(users),
              db::block_id(fetched->start_height),
              block.timestamp,
              miner_tx_hash,
              block.miner_tx,
              *(indices.begin()),
              reader
            );

            if (opts.untrusted_daemon)
            {
              if (block.prev_id != blockchain.back())
                MONERO_THROW(error::bad_blockchain, "A blocks prev_id does not match");

              update_window(pow_window.pow_timestamps);
              update_window(pow_window.cumulative_diffs);

              while (BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW < pow_window.median_timestamps.size())
                pow_window.median_timestamps.erase(pow_window.median_timestamps.begin());

              // longhash takes a while, check is_running
              if (self.stop_)
                return false;

              diff = cryptonote::next_difficulty(pow_window.pow_timestamps, pow_window.cumulative_diffs, get_target_time(db::block_id(fetched->start_height)));

              // skip POW hashing if done previously
              if (disk && last_pow < db::block_id(fetched->start_height))
              {
                if (!verify_timestamp(block.timestamp, pow_window.median_timestamps))
                  MONERO_THROW(error::bad_blockchain, "Block failed timestamp check - possible chain forgery");

                const crypto::hash pow =
                  get_block_longhash(get_block_hashing_blob(block), db::block_id(fetched->start_height), block.major_version, *disk, initial_height, epee::to_span(blockchain));
                if (!cryptonote::check_hash(pow, diff))
                  MONERO_THROW(error::bad_blockchain, "Block had too low difficulty");
              }
            }

            indices.remove_prefix(1);
            if (txes.size() != indices.size())
              throw std::runtime_error{"Bad daemon respnse - need same number of txes and indices"};

            for (auto tx_data : boost::combine(block.tx_hashes, txes, indices))
            {
              if (opts.untrusted_daemon)
              {
                if (cryptonote::get_transaction_hash(boost::get<1>(tx_data)) != boost::get<0>(tx_data))
                  MONERO_THROW(error::bad_blockchain, "Hash of transaction does not match hash in block");
              }

              scan_transaction(
                epee::to_mut_span(users),
                db::block_id(fetched->start_height),
                block.timestamp,
                boost::get<0>(tx_data),
                boost::get<1>(tx_data),
                boost::get<2>(tx_data),
                reader
              );
            }

            if (opts.untrusted_daemon)
            {
              const auto last_difficulty =
                pow_window.cumulative_diffs.empty() ?
                  db::block_difficulty::unsigned_int(0) : pow_window.cumulative_diffs.back();

              pow_window.pow_timestamps.push_back(block.timestamp);
              pow_window.median_timestamps.push_back(block.timestamp);
              pow_window.cumulative_diffs.push_back(diff + last_difficulty);
              new_pow.push_back(db::pow_sync{block.timestamp});
              new_pow.back().cumulative_diff.set_difficulty(pow_window.cumulative_diffs.back());
            }
            blockchain.push_back(cryptonote::get_block_hash(block));
          } // for each block

          reader.reader = std::error_code{common_error::kInvalidArgument}; // cleanup reader before next write

          MINFO("Thread " << thread_n << " processed " << blockchain.size() << " blocks(s) @ height " << fetched->start_height << " against " << users.size() << " account(s)");

          if (!store(self.io_, client, self.webhooks_, epee::to_span(blockchain), epee::to_span(users), epee::to_span(new_pow)))
            return false;

          // TODO         
          if (opts.untrusted_daemon && leader_thread && fetched->start_height % 4 == 0 && last_pow < db::block_id(fetched->start_height))
          {
            MINFO("On chain with hash " << blockchain.back() << " and difficulty " << diff << " at height " << fetched->start_height);
          }

          for (account& user : users)
            user.updated(db::block_id(fetched->start_height));
        }
      }

      return false;
    } // end scan_loop

  namespace
  {
    /*!
      Launches `thread_count` threads to run `scan_loop`, and then polls for
      active account changes in background
    */
    void check_loop(scanner_sync& self, db::storage disk, rpc::context& ctx, const std::size_t thread_count, const std::string& lws_server_addr, std::string lws_server_pass, std::vector<lws::account> users, std::vector<db::account_id> active, const scanner_options& opts)
    {
      assert(users.size() == active.size());
      assert(thread_count || !lws_server_addr.empty());
      assert(!thread_count || !users.empty());

      std::vector<boost::thread> threads{};
      threads.reserve(thread_count);

      std::vector<std::shared_ptr<rpc::scanner::queue>> queues;
      queues.resize(thread_count);

      {
        struct join_
        {
          scanner_sync& self;
          rpc::context& ctx;
          std::vector<std::shared_ptr<rpc::scanner::queue>>& queues;
          std::vector<boost::thread>& threads;

          ~join_() noexcept
          {
            self.stop();
            if (self.has_shutdown())
              ctx.raise_abort_process();
            else
              ctx.raise_abort_scan();

            for (const auto& queue : queues)
            {
              if (queue)
                queue->stop();
            }
            for (auto& thread : threads)
              thread.join();
          }
        } join{self, ctx, queues, threads};

        /*
          The algorithm here is extremely basic. Users are divided evenly amongst
          the configurable thread count, and grouped by scan height. If an old
          account appears, some accounts (grouped on that thread) will be delayed
          in processing waiting for that account to catch up. Its not the greatest,
          but this "will have to do" - but we're getting closer to fixing that
          too.

          Another "issue" is that each thread works independently instead of more
          cooperatively for scanning. This requires a bit more synchronization, so
          was left for later. Its likely worth doing to reduce the number of
          transfers from the daemon, and the bottleneck on the writes into LMDB.
        */

        self.stop_ = false;

        boost::thread::attributes attrs;
        attrs.set_stack_size(THREAD_STACK_SIZE);

        MINFO("Starting scan loops on " << thread_count << " thread(s) with " << users.size() << " account(s)");

        if (opts.block_depth_threading)
        {
          // Get current blockchain height
          const db::block_id current_height = MONERO_UNWRAP(MONERO_UNWRAP(disk.start_read()).get_last_block()).id;
          
          // Calculate blockdepth for each account and create pairs
          struct account_depth {
            std::size_t index;
            std::uint64_t blockdepth;
          };
          std::vector<account_depth> account_depths;
          account_depths.reserve(users.size());
          
          std::uint64_t total_blockdepth = 0;
          for (std::size_t i = 0; i < users.size(); ++i)
          {
            const std::uint64_t raw_blockdepth = std::uint64_t(current_height) - std::uint64_t(users[i].scan_height());
            const std::uint64_t blockdepth = std::max(raw_blockdepth, std::uint64_t(MINIMUM_BLOCK_DEPTH));
            account_depths.push_back(account_depth{i, blockdepth});
            total_blockdepth += blockdepth;
          }
          
          // Sort by blockdepth (smallest first)
          std::sort(account_depths.begin(), account_depths.end(),
            [](const account_depth& a, const account_depth& b) {
              return a.blockdepth < b.blockdepth;
            });
          
          // Calculate target blockdepth per thread
          const std::uint64_t blockdepth_per_thread = total_blockdepth / thread_count;
          
          MINFO("Using block-depth threading: total_blockdepth=" << total_blockdepth 
                << ", blockdepth_per_thread=" << blockdepth_per_thread);
          
          // Prepare thread assignment data structure
          std::vector<std::vector<lws::account>> thread_assignments(thread_count);
          
          // Assign accounts to threads based on cumulative blockdepth
          std::size_t current_thread = 0;
          std::uint64_t current_thread_depth = 0;
          
          for (const auto& ad : account_depths)
          {
            // If adding this account would exceed the target and we're not on the last thread
            if (current_thread_depth >= blockdepth_per_thread && current_thread < thread_count - 1)
            {
              ++current_thread;
              current_thread_depth = 0;
            }
            
            thread_assignments[current_thread].push_back(std::move(users[ad.index]));
            current_thread_depth += ad.blockdepth;
          }
          
          // Create threads with assigned accounts
          for (std::size_t i = 0; i < queues.size(); ++i)
          {
            queues[i] = std::make_shared<rpc::scanner::queue>();

            auto data = std::make_shared<thread_data>(
              MONERO_UNWRAP(ctx.connect()), disk.clone(), std::move(thread_assignments[i]), queues[i], opts
            );
            threads.emplace_back(attrs, std::bind(&do_scan_loop, std::ref(self), std::move(data), i));
          }
        }
        else
        {
          // Original algorithm: sort by height and divide evenly by count
          std::sort(users.begin(), users.end(), by_height{});

          for (std::size_t i = 0; i < queues.size(); ++i)
          {
            queues[i] = std::make_shared<rpc::scanner::queue>();

            // this can create threads with no active accounts, they just wait
            const std::size_t count = users.size() / (queues.size() - i);
            std::vector<lws::account> thread_users{
              std::make_move_iterator(users.end() - count), std::make_move_iterator(users.end())
            };
            users.erase(users.end() - count, users.end());

            auto data = std::make_shared<thread_data>(
              MONERO_UNWRAP(ctx.connect()), disk.clone(), std::move(thread_users), queues[i], opts
            );
            threads.emplace_back(attrs, std::bind(&do_scan_loop, std::ref(self), std::move(data), i));
          }
        }

        users.clear();
        users.shrink_to_fit();

        auto server = std::make_shared<rpc::scanner::server>(
          self.io_,
          disk.clone(),
          MONERO_UNWRAP(ctx.connect()),
          queues,
          std::move(active),
          self.webhooks_.ssl_context()
        );

        rpc::scanner::server::start_user_checking(server);
        if (!lws_server_addr.empty())
          rpc::scanner::server::start_acceptor(server, lws_server_addr, std::move(lws_server_pass));

        // This is a hack to prevent racy shutdown
        boost::asio::post(self.io_, [&self] () { if (!self.is_running()) self.stop(); });

        // Blocks until sigint, local scanner issue, storage issue, or exception
        self.io_.restart();
        self.io_.run();

        rpc::scanner::server::stop(server);
      } // block until all threads join

      // Make sure server stops because we could re-start after blockchain sync
      self.io_.restart();
      self.io_.poll();
    }

    template<typename R, typename Q>
    expect<typename R::response> fetch_chain(scanner_sync& self, rpc::client& client, const char* endpoint, const Q& req)
    {
      expect<void> sent{lws::error::daemon_timeout};

      epee::byte_slice msg = rpc::client::make_message(endpoint, req);
      auto start = std::chrono::steady_clock::now();

      while (!(sent = client.send(std::move(msg), std::chrono::seconds{1})))
      {
        // Run possible SIGINT handler
        self.io_.poll_one();
        self.io_.restart();
        if (self.has_shutdown())
          return {lws::error::signal_abort_process};

        if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
          return {lws::error::daemon_timeout};

        if (!sent.matches(std::errc::timed_out))
          return sent.error();
      }

      expect<std::string> resp{lws::error::daemon_timeout};
      start = std::chrono::steady_clock::now();

      while (!(resp = client.get_message(std::chrono::seconds{1})))
      {
        // Run possible SIGINT handler
        self.io_.poll_one();
        self.io_.restart();
        if (self.has_shutdown())
          return {lws::error::signal_abort_process};

        if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
          return {lws::error::daemon_timeout};

        if (!resp.matches(std::errc::timed_out))
          return resp.error();
      }
      return rpc::parse_json_response<R>(std::move(*resp));
    }

    // does not validate blockchain hashes
    expect<rpc::client> sync_quick(scanner_sync& self, db::storage disk, rpc::client client, bool regtest)
    {
      MINFO("Starting blockchain sync with daemon");

      cryptonote::rpc::GetHashesFast::Request req{};
      req.start_height = 0;
      req.known_hashes = MONERO_UNWRAP(MONERO_UNWRAP(disk.start_read()).get_chain_sync());

      for (;;)
      {
        if (req.known_hashes.empty())
          return {lws::error::bad_blockchain};

        auto resp = fetch_chain<rpc::get_hashes_fast>(self, client, "get_hashes_fast", req);
        if (!resp)
          return resp.error();

        //
        // exit loop if it appears we have synced to top of chain
        //
        if (resp->hashes.size() <= 1 || resp->hashes.back() == req.known_hashes.front())
          return {std::move(client)};

        MONERO_CHECK(disk.sync_chain(db::block_id(resp->start_height), epee::to_span(resp->hashes), regtest));

        req.known_hashes.erase(req.known_hashes.begin(), --(req.known_hashes.end()));
        for (std::size_t num = 0; num < 10; ++num)
        {
          if (resp->hashes.empty())
            break;

          req.known_hashes.insert(--(req.known_hashes.end()), resp->hashes.back());
        }
      }

      return {std::move(client)};
    } 
 
    // validates blockchain hashes
    expect<rpc::client> sync_full(scanner_sync& self, db::storage disk, rpc::client client)
    {
      MINFO("Starting blockchain sync with daemon");

      cryptonote::rpc::GetBlocksFast::Request req{};
      req.start_height = 0;
      req.block_ids = MONERO_UNWRAP(MONERO_UNWRAP(disk.start_read()).get_pow_sync());
      req.prune = true;

      std::vector<crypto::hash> new_hashes{};
      std::vector<db::pow_sync> new_pow{};
      for (;;)
      {
        if (req.block_ids.empty())
          return {lws::error::bad_blockchain};

        auto resp = fetch_chain<rpc::get_blocks_fast>(self, client, "get_blocks_fast", req);
        if (!resp)
          return resp.error();

        if (resp->blocks.empty())
          return {error::bad_daemon_response};

        crypto::hash hash{};
        if (!cryptonote::get_block_hash(resp->blocks.front().block, hash))
          return {lws::error::bad_blockchain};

        //
        // exit loop if it appears we have synced to top of chain
        //
        const db::block_info last_checkpoint = db::storage::get_last_checkpoint();
        if (resp->blocks.size() <= 1)
        {
          // error if not past last checkpoint
          const auto expected_hash =
            MONERO_UNWRAP(disk.start_read()).get_block_hash(db::block_id(resp->start_height));
          if (!expected_hash || *expected_hash != hash || db::block_id(resp->start_height) < last_checkpoint.id)
            return {error::bad_daemon_response};
          return {std::move(client)};
        }

        // genesis block must be present as last entry
        req.block_ids.erase(req.block_ids.begin(), --(req.block_ids.end()));

        auto pow_window =
          MONERO_UNWRAP(MONERO_UNWRAP(disk.start_read()).get_pow_window(db::block_id(resp->start_height)));

        // overlap check performed in db::storage::pow_sync
        new_hashes.clear();
        new_pow.clear();
        new_hashes.reserve(resp->blocks.size());
        new_pow.reserve(resp->blocks.size());
        new_hashes.push_back(hash);
        new_pow.push_back(db::pow_sync{resp->blocks.front().block.timestamp});

        // skip overlap block
        db::block_difficulty::unsigned_int diff = 0;
        for (std::size_t i = 1; i < resp->blocks.size(); ++i)
        {
          const auto& block = resp->blocks[i].block;
          const db::block_id height = db::block_id(resp->start_height + i);
 
          // important check, ensure we haven't deviated from chain
          if (block.prev_id != hash)
            return {lws::error::bad_blockchain};
 
          // compute block id hash
          if (!cryptonote::get_block_hash(block, hash))
            return {lws::error::bad_blockchain};

          req.block_ids.push_front(hash);
          update_window(pow_window.pow_timestamps);
          update_window(pow_window.cumulative_diffs);

          while (BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW < pow_window.median_timestamps.size())
            pow_window.median_timestamps.erase(pow_window.median_timestamps.begin());

          // longhash takes a while, check is_running
          if (self.has_shutdown())
            return {error::signal_abort_process}; 

          diff = cryptonote::next_difficulty(pow_window.pow_timestamps, pow_window.cumulative_diffs, get_target_time(height));

          // skip POW hashing when sync is within checkpoint
          // storage::sync_pow(...) currently verifies checkpoint hashes
          if (last_checkpoint.id < height)
          {
            if (!verify_timestamp(block.timestamp, pow_window.median_timestamps))
            {
              MERROR("Block failed timestamp check - possible chain forgery");
              return {error::bad_blockchain};
            }
            const crypto::hash pow =
              get_block_longhash(get_block_hashing_blob(block), height, block.major_version, disk, db::block_id(resp->start_height), epee::to_span(new_hashes));

            if (!cryptonote::check_hash(pow, diff))
            {
              MERROR("Block " << std::uint64_t(height) << "had too low difficulty");
              return {error::bad_blockchain};
            }
          }

          const auto last_difficulty =
            pow_window.cumulative_diffs.empty() ?
              db::block_difficulty::unsigned_int(0) : pow_window.cumulative_diffs.back();

          pow_window.pow_timestamps.push_back(block.timestamp);
          pow_window.median_timestamps.push_back(block.timestamp);
          pow_window.cumulative_diffs.push_back(diff + last_difficulty);
          new_hashes.push_back(hash);
          new_pow.push_back(db::pow_sync{block.timestamp});
          new_pow.back().cumulative_diff.set_difficulty(pow_window.cumulative_diffs.back());
        } // for every tx in block

        MONERO_CHECK(disk.sync_pow(db::block_id(resp->start_height), epee::to_span(new_hashes), epee::to_span(new_pow)));
        MINFO("Verified up to block " << (resp->start_height + new_hashes.size() - 1) << " with hash " << hash << " and difficulty " << diff);

      } // for until sync

      return {std::move(client)};
    }
  } // anonymous

  bool user_data::store(boost::asio::io_context& io, db::storage& disk, rpc::client& client, net::http::client& webhook, const epee::span<const crypto::hash> chain, const epee::span<const lws::account> users, const epee::span<const db::pow_sync> pow)
  {
    if (users.empty())
      return true;
    if (!std::is_sorted(users.begin(), users.end(), by_height{}))
      throw std::logic_error{"users must be sorted!"};

    auto updated = disk.update(users[0].scan_height(), chain, users, pow);
    if (!updated)
    {
      if (updated == lws::error::blockchain_reorg)
      {
        MINFO("Blockchain reorg detected, resetting state");
        return false;
      }
      MONERO_THROW(updated.error(), "Failed to update accounts on disk");
    }

    send_payment_hook(io, client, webhook, epee::to_span(updated->confirm_pubs));
    send_spend_hook(io, client, webhook, epee::to_span(updated->spend_pubs));
    if (updated->accounts_updated != users.size())
    {
      MWARNING("Only updated " << updated->accounts_updated << " account(s) out of " << users.size() << ", resetting");
      return false;
    }

    // Publish when all scan threads have past this block
    // only address is printed from users, so height doesn't need updating
    if (!chain.empty() && client.has_publish())
      rpc::publish_scanned(client, chain[chain.size() - 1], epee::to_span(users));

    return true;
  }

  bool user_data::operator()(boost::asio::io_context& io, rpc::client& client, net::http::client& webhook, const epee::span<const crypto::hash> chain, const epee::span<const lws::account> users, const epee::span<const db::pow_sync> pow)
  {
    return store(io, disk_, client, webhook, chain, users, pow);
  } 

  expect<rpc::client> scanner::sync(rpc::client client, const bool untrusted_daemon, const bool regtest)
  {
    if (has_shutdown())
      MONERO_THROW(common_error::kInvalidArgument, "this has shutdown");
    if (untrusted_daemon)
      return sync_full(sync_, disk_.clone(), std::move(client));
    return sync_quick(sync_, disk_.clone(), std::move(client), regtest);
  }

  void scanner::run(rpc::context ctx, std::size_t thread_count, const std::string& lws_server_addr, std::string lws_server_pass, const scanner_options& opts)
  {
    if (has_shutdown())
      MONERO_THROW(common_error::kInvalidArgument, "this has shutdown");
    if (!lws_server_addr.empty() && (opts.max_subaddresses || opts.untrusted_daemon))
      MONERO_THROW(error::configuration, "Cannot use remote scanner with subaddresses or untrusted daemon");

    if (lws_server_addr.empty())
      thread_count = std::max(std::size_t(1), thread_count);

    /*! \NOTE Be careful about references and lifetimes of the callbacks. The
      ones below are safe because no `io_context::run()` call is after the
      destruction of the references. */
    boost::asio::steady_timer rate_timer{sync_.io_};
    class rate_updater
    {
      boost::asio::io_context& io_;
      boost::asio::steady_timer& rate_timer_;
      rpc::context& ctx_;
      const std::chrono::minutes rate_interval_;

    public:
      explicit rate_updater(boost::asio::io_context& io, boost::asio::steady_timer& rate_timer, rpc::context& ctx)
        : io_(io), rate_timer_(rate_timer), ctx_(ctx), rate_interval_(ctx.cache_interval())
      {}

      void operator()(const boost::system::error_code& error = {}) const
      {
        const expect<void> status = ctx_.retrieve_rates_async(io_);
        if (!status)
          MERROR("Unable to retrieve exchange rates: " << status.error());
        rate_timer_.expires_after(rate_interval_);
        rate_timer_.async_wait(*this);
      }

      std::chrono::minutes rate_interval() const noexcept { return rate_interval_; }
    };

    {
      rate_updater updater{sync_.io_, rate_timer, ctx};
      if (std::chrono::minutes{0} < updater.rate_interval())
        updater();
    }

    rpc::client client{};

    for (;;)
    {
      std::vector<db::account_id> active;
      std::vector<lws::account> users;

      if (thread_count)
      {
        MINFO("Retrieving current active account list");

        auto reader = MONERO_UNWRAP(disk_.start_read());
        auto accounts = MONERO_UNWRAP(
          reader.get_accounts(db::account_status::active)
        );

        for (db::account user : accounts.make_range())
        {
          users.emplace_back(MONERO_UNWRAP(reader.get_full_account(user)));
          active.insert(
            std::lower_bound(active.begin(), active.end(), user.id), user.id
          );
        }

        reader.finish_read();
      } // cleanup DB reader

      if (thread_count && users.empty())
      {
        MINFO("No active accounts");

        boost::asio::steady_timer poll{sync_.io_};
        poll.expires_after(rpc::scanner::account_poll_interval);
        const auto ready = poll.async_wait(boost::asio::use_future);

        /* The exchange rates timer could run while waiting, so ensure that
          the correct timer was run. */
        while (!has_shutdown() && ready.wait_for(std::chrono::seconds{0}) == std::future_status::timeout)
        {
          sync_.io_.restart();
          sync_.io_.run_one();
        }
      }
      else
        check_loop(sync_, disk_.clone(), ctx, thread_count, lws_server_addr, lws_server_pass, std::move(users), std::move(active), opts);

      if (has_shutdown())
        return;

      if (!client)
        client = MONERO_UNWRAP(ctx.connect());

      expect<rpc::client> synced = sync(std::move(client), opts.untrusted_daemon, opts.regtest);
      if (!synced)
      {
        if (!synced.matches(std::errc::timed_out))
          MONERO_THROW(synced.error(), "Unable to sync blockchain");

        MWARNING("Failed to connect to daemon at " << ctx.daemon_address());
      }
      else
        client = std::move(*synced);
    } // while scanning
  }
} // lws
