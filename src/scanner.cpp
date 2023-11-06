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
#include "crypto/crypto.h"                            // monero/src
#include "crypto/wallet/crypto.h"                     // monero/src
#include "cryptonote_basic/cryptonote_basic.h"        // monero/src
#include "cryptonote_basic/cryptonote_format_utils.h" // monero/src
#include "db/account.h"
#include "db/data.h"
#include "error.h"
#include "misc_log_ex.h"           // monero/contrib/epee/include
#include "net/net_parse_helpers.h"
#include "net/net_ssl.h"           // monero/contrib/epee/include
#include "rpc/daemon_messages.h"   // monero/src
#include "rpc/daemon_zmq.h"
#include "rpc/json.h"
#include "rpc/message_data_structs.h" // monero/src
#include "rpc/webhook.h"
#include "util/source_location.h"
#include "util/transactions.h"

#include "serialization/json_object.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"

namespace lws
{
  std::atomic<bool> scanner::running{true};

  // Not in `rates.h` - defaulting to JSON output seems odd
  std::ostream& operator<<(std::ostream& out, lws::rates const& src)
  {
    wire::json_stream_writer dest{out};
    lws::write_bytes(dest, src);
    dest.finish();
    return out;
  }

  namespace
  {
    namespace net = epee::net_utils;

    constexpr const std::chrono::seconds account_poll_interval{10};
    constexpr const std::chrono::minutes block_rpc_timeout{2};
    constexpr const std::chrono::seconds send_timeout{30};
    constexpr const std::chrono::seconds sync_rpc_timeout{30};

    struct thread_sync
    {
      boost::mutex sync;
      boost::condition_variable user_poll;
      std::atomic<bool> update;
    };

    struct options
    {
      net::ssl_verification_t webhook_verify;
      bool enable_subaddresses;
    };

    struct thread_data
    {
      explicit thread_data(rpc::client client, db::storage disk, std::vector<lws::account> users, options opts)
        : client(std::move(client)), disk(std::move(disk)), users(std::move(users)), opts(opts)
      {}

      rpc::client client;
      db::storage disk;
      std::vector<lws::account> users;
      options opts;
    };

    // until we have a signal-handler safe notification system
    void checked_wait(const std::chrono::nanoseconds wait)
    {
      static constexpr const std::chrono::milliseconds interval{500};

      const auto start = std::chrono::steady_clock::now();
      while (scanner::is_running())
      {
        const auto current = std::chrono::steady_clock::now() - start;
        if (wait <= current)
          break;
        const auto sleep_time = std::min(wait - current, std::chrono::nanoseconds{interval});
        boost::this_thread::sleep_for(boost::chrono::nanoseconds{sleep_time.count()});
      }
    }

    bool is_new_block(std::string&& chain_msg, db::storage& disk, const account& user)
    {
      const auto chain = rpc::minimal_chain_pub::from_json(std::move(chain_msg));
      if (!chain)
      {
        MERROR("Unable to parse blockchain notification: " << chain.error());
        return false;
      }

      if (user.scan_height() < db::block_id(chain->top_block_height))
        return true;

      auto reader = disk.start_read();
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

    void send_payment_hook(rpc::client& client, const epee::span<const db::webhook_tx_confirmation> events, net::ssl_verification_t verify_mode)
    {
      rpc::send_webhook(client, events, "json-full-payment_hook:", "msgpack-full-payment_hook:", std::chrono::seconds{5}, verify_mode);
    }

    struct by_height
    {
      bool operator()(account const& left, account const& right) const noexcept
      {
        return left.scan_height() < right.scan_height();
      }
    };

    struct add_spend
    {
      void operator()(lws::account& user, const db::spend& spend) const
      { user.add_spend(spend); }
    };
    struct add_output
    {
      bool operator()(lws::account& user, const db::output& out) const
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
      net::ssl_verification_t verify_mode_;
      std::unordered_map<crypto::hash, crypto::hash> txpool_;

      bool operator()(lws::account& user, const db::output& out)
      {
        /* Upstream monerod does not send all fields for a transaction, so
           mempool notifications cannot compute tx_hash correctly (it is not
           sent separately, a further blunder). Instead, if there are matching
           outputs with webhooks, fetch mempool to compare tx_prefix_hash and
           then use corresponding tx_hash. */
        const db::webhook_key key{user.id(), db::webhook_type::tx_confirmation};
        std::vector<db::webhook_value> hooks{};
        {
          auto reader = disk_.start_read();
          if (!reader)
          {
            MERROR("Unable to lookup webhook on tx in pool: " << reader.error().message());
            return false;
          }
          auto found = reader->find_webhook(key, out.payment_id.short_);
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
        send_payment_hook(client_, epee::to_span(events), verify_mode_);
        return true;
      }
    };

    struct subaddress_reader
    {
      expect<db::storage_reader> reader;
      db::cursor::subaddress_indexes cur;

      subaddress_reader(db::storage const& disk, const bool enable_subaddresses)
        : reader(common_error::kInvalidArgument), cur(nullptr)
      {
        if (enable_subaddresses)
        {
          reader = disk.start_read();
          if (!reader)
            MERROR("Subadress lookup failure: " << reader.error().message());
        }
      }
    };

    void scan_transaction_base(
      epee::span<lws::account> users,
      const db::block_id height,
      const std::uint64_t timestamp,
      crypto::hash const& tx_hash,
      cryptonote::transaction const& tx,
      std::vector<std::uint64_t> const& out_ids,
      subaddress_reader& reader,
      std::function<void(lws::account&, const db::spend&)> spend_action,
      std::function<bool(lws::account&, const db::output&)> output_action)
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
          crypto::key_derivation active_derived;

          // inspect the additional and traditional keys
          for (std::size_t attempt = 0; attempt < 2; ++attempt)
          {
            if (attempt == 0)
              active_derived = derived;
            else if (!additional_derivations.empty())
              active_derived = additional_derivations.at(index);
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

          if (extra_nonce)
          {
            if (!payment_id.first && cryptonote::get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce->nonce, payment_id.second.short_))
            {
              payment_id.first = sizeof(crypto::hash8);
              lws::decrypt_payment_id(payment_id.second.short_, active_derived);
            }
          }

          const bool added = output_action(
            user,
            db::output{
              db::transaction_link{height, tx_hash},
              db::output::spend_meta_{
                db::output_id{out.amount, out_ids.at(index)},
                amount,
                mixin,
                boost::numeric_cast<std::uint32_t>(index),
                key.pub_key
              },
              timestamp,
              tx.unlock_time,
              *prefix_hash,
              out_pub_key,
              mask,
              {0, 0, 0, 0, 0, 0, 0}, // reserved bytes
              db::pack(ext, payment_id.first),
              payment_id.second,
              tx.rct_signatures.txnFee,
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

    void scan_transactions(std::string&& txpool_msg, epee::span<lws::account> users, db::storage const& disk, rpc::client& client, const options& opts)
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

      subaddress_reader reader{disk, opts.enable_subaddresses};
      send_webhook sender{disk, client, opts.webhook_verify};
      for (const auto& tx : parsed->txes)
        scan_transaction_base(users, db::block_id::txpool, time, crypto::hash{}, tx, fake_outs, reader, null_spend{}, sender);
    }

    void update_rates(rpc::context& ctx)
    {
      const expect<boost::optional<lws::rates>> new_rates = ctx.retrieve_rates();
      if (!new_rates)
        MERROR("Failed to retrieve exchange rates: " << new_rates.error().message());
      else if (*new_rates)
        MINFO("Updated exchange rates: " << *(*new_rates));
    }

    void scan_loop(thread_sync& self, std::shared_ptr<thread_data> data) noexcept
    {
      try
      {
        // boost::thread doesn't support move-only types + attributes
        rpc::client client{std::move(data->client)};
        db::storage disk{std::move(data->disk)};
        std::vector<lws::account> users{std::move(data->users)};
        const options opts = std::move(data->opts);

        assert(!users.empty());
        assert(std::is_sorted(users.begin(), users.end(), by_height{}));

        data.reset();

        struct stop_
        {
          thread_sync& self;
          ~stop_() noexcept
          {
            self.update = true;
            self.user_poll.notify_one();
          }
        } stop{self};

        // RPC server assumes that `start_height == 0` means use
        // block ids. This technically skips genesis block.
        cryptonote::rpc::GetBlocksFast::Request req{};
        req.start_height = std::uint64_t(users.begin()->scan_height());
        req.start_height = std::max(std::uint64_t(1), req.start_height);
        req.prune = true;

        epee::byte_slice block_request = rpc::client::make_message("get_blocks_fast", req);
        if (!send(client, block_request.clone()))
          return;

        std::vector<crypto::hash> blockchain{};

        while (!self.update && scanner::is_running())
        {
          blockchain.clear();

          auto resp = client.get_message(block_rpc_timeout);
          if (!resp)
          {
            const bool timeout = resp.matches(std::errc::timed_out);
            if (timeout)
              MWARNING("Block retrieval timeout, resetting scanner");
            if (timeout || resp.matches(std::errc::interrupted))
              return;
            MONERO_THROW(resp.error(), "Failed to retrieve blocks from daemon");
          }

          auto fetched = rpc::parse_json_response<rpc::get_blocks_fast>(std::move(*resp));
          if (!fetched)
          {
            MERROR("Failed to retrieve next blocks: " << fetched.error().message() << ". Resetting state and trying again");
            return;
          }

          if (fetched->blocks.empty())
            throw std::runtime_error{"Daemon unexpectedly returned zero blocks"};

          if (fetched->start_height != req.start_height)
          {
            MWARNING("Daemon sent wrong blocks, resetting state");
            return;
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
                return; // reset entire state (maybe shutdown)

              if (!new_pubs)
                break; // exit wait for block loop, and try fetching new blocks

              // put txpool messages before block messages
              static_assert(rpc::client::topic::block < rpc::client::topic::txpool, "bad sort");
              std::sort(new_pubs->begin(), new_pubs->end(), std::greater<>{});

              // process txpool first
              auto message = new_pubs->begin();
              for ( ; message != new_pubs->end(); ++message)
              {
                if (message->first != rpc::client::topic::txpool)
                  break; // inner for loop
                scan_transactions(std::move(message->second), epee::to_mut_span(users), disk, client, opts);
              }

              for ( ; message != new_pubs->end(); ++message)
              {
                if (message->first == rpc::client::topic::block && is_new_block(std::move(message->second), disk, users.front()))
                  wait_for_block = false;
              }
            } // wait for block

            // request next chunk of blocks
            if (!send(client, block_request.clone()))
              return;
            continue; // to next get_blocks_fast read
          } // if only one block was fetched

          // request next chunk of blocks
          if (!send(client, block_request.clone()))
            return;

          if (fetched->blocks.size() != fetched->output_indices.size())
            throw std::runtime_error{"Bad daemon response - need same number of blocks and indices"};

          blockchain.push_back(cryptonote::get_block_hash(fetched->blocks.front().block));

          auto blocks = epee::to_span(fetched->blocks);
          auto indices = epee::to_span(fetched->output_indices);

          if (fetched->start_height != 1)
          {
            // skip overlap block
            blocks.remove_prefix(1);
            indices.remove_prefix(1);
          }
          else
            fetched->start_height = 0;

          subaddress_reader reader{disk, opts.enable_subaddresses};
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

            indices.remove_prefix(1);
            if (txes.size() != indices.size())
              throw std::runtime_error{"Bad daemon respnse - need same number of txes and indices"};

            for (auto tx_data : boost::combine(block.tx_hashes, txes, indices))
            {
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

            blockchain.push_back(cryptonote::get_block_hash(block));
          } // for each block

          reader.reader = std::error_code{common_error::kInvalidArgument}; // cleanup reader before next write
          auto updated = disk.update(
            users.front().scan_height(), epee::to_span(blockchain), epee::to_span(users)
          );
          if (!updated)
          {
            if (updated == lws::error::blockchain_reorg)
            {
              MINFO("Blockchain reorg detected, resetting state");
              return;
            }
            MONERO_THROW(updated.error(), "Failed to update accounts on disk");
          }

          MINFO("Processed " << blocks.size() << " block(s) against " << users.size() << " account(s)");
          send_payment_hook(client, epee::to_span(updated->second), opts.webhook_verify);
          if (updated->first != users.size())
          {
            MWARNING("Only updated " << updated->first << " account(s) out of " << users.size() << ", resetting");
            return;
          }

          for (account& user : users)
            user.updated(db::block_id(fetched->start_height));
        }
      }
      catch (std::exception const& e)
      {
        scanner::stop();
        MERROR(e.what());
      }
      catch (...)
      {
        scanner::stop();
        MERROR("Unknown exception");
      }
    }

    /*!
      Launches `thread_count` threads to run `scan_loop`, and then polls for
      active account changes in background
    */
    void check_loop(db::storage disk, rpc::context& ctx, std::size_t thread_count, std::vector<lws::account> users, std::vector<db::account_id> active, const options opts)
    {
      assert(0 < thread_count);
      assert(0 < users.size());

      thread_sync self{};
      std::vector<boost::thread> threads{};

      struct join_
      {
        thread_sync& self;
        std::vector<boost::thread>& threads;
        rpc::context& ctx;

        ~join_() noexcept
        {
          self.update = true;
          ctx.raise_abort_scan();
          for (auto& thread : threads)
            thread.join();
        }
      } join{self, threads, ctx};

      /*
        The algorithm here is extremely basic. Users are divided evenly amongst
        the configurable thread count, and grouped by scan height. If an old
        account appears, some accounts (grouped on that thread) will be delayed
        in processing waiting for that account to catch up. Its not the greatest,
        but this "will have to do" for the first cut.
        Its not expected that many people will be running
        "enterprise level" of nodes where accounts are constantly added.

        Another "issue" is that each thread works independently instead of more
        cooperatively for scanning. This requires a bit more synchronization, so
        was left for later. Its likely worth doing to reduce the number of
        transfers from the daemon, and the bottleneck on the writes into LMDB.

        If the active user list changes, all threads are stopped/joined, and
        everything is re-started.
      */

      boost::thread::attributes attrs;
      attrs.set_stack_size(THREAD_STACK_SIZE);

      threads.reserve(thread_count);
      std::sort(users.begin(), users.end(), by_height{});

      MINFO("Starting scan loops on " << std::min(thread_count, users.size()) << " thread(s) with " << users.size() << " account(s)");

      while (!users.empty() && --thread_count)
      {
        const std::size_t per_thread = std::max(std::size_t(1), users.size() / (thread_count + 1));
        const std::size_t count = std::min(per_thread, users.size());
        std::vector<lws::account> thread_users{
          std::make_move_iterator(users.end() - count), std::make_move_iterator(users.end())
        };
        users.erase(users.end() - count, users.end());

        rpc::client client = MONERO_UNWRAP(ctx.connect());
        client.watch_scan_signals();

        auto data = std::make_shared<thread_data>(
          std::move(client), disk.clone(), std::move(thread_users), opts
        );
        threads.emplace_back(attrs, std::bind(&scan_loop, std::ref(self), std::move(data)));
      }

      if (!users.empty())
      {
        rpc::client client = MONERO_UNWRAP(ctx.connect());
        client.watch_scan_signals();

        auto data = std::make_shared<thread_data>(
          std::move(client), disk.clone(), std::move(users), opts
        );
        threads.emplace_back(attrs, std::bind(&scan_loop, std::ref(self), std::move(data)));
      }

      auto last_check = std::chrono::steady_clock::now();

      lmdb::suspended_txn read_txn{};
      db::cursor::accounts accounts_cur{};
      boost::unique_lock<boost::mutex> lock{self.sync};

      while (scanner::is_running())
      {
        update_rates(ctx);

        for (;;)
        {
          //! \TODO use signalfd + ZMQ? Windows is the difficult case...
          self.user_poll.wait_for(lock, boost::chrono::seconds{1});
          if (self.update || !scanner::is_running())
            return;
          auto this_check = std::chrono::steady_clock::now();
          if (account_poll_interval <= (this_check - last_check))
          {
            last_check = this_check;
            break;
          }
        }

        auto reader = disk.start_read(std::move(read_txn));
        if (!reader)
        {
          if (reader.matches(std::errc::no_lock_available))
          {
            MWARNING("Failed to open DB read handle, retrying later");
            continue;
          }
          MONERO_THROW(reader.error(), "Failed to open DB read handle");
        }

        auto current_users = MONERO_UNWRAP(
          reader->get_accounts(db::account_status::active, std::move(accounts_cur))
        );
        if (current_users.count() != active.size())
        {
          MINFO("Change in active user accounts detected, stopping scan threads...");
          return;
        }

        for (auto user = current_users.make_iterator(); !user.is_end(); ++user)
        {
          const db::account_id user_id = user.get_value<MONERO_FIELD(db::account, id)>();
          if (!std::binary_search(active.begin(), active.end(), user_id))
          {
            MINFO("Change in active user accounts detected, stopping scan threads...");
            return;
          }
        }

        read_txn = reader->finish_read();
        accounts_cur = current_users.give_cursor();
      } // while scanning
    }
  } // anonymous

  expect<rpc::client> scanner::sync(db::storage disk, rpc::client client)
  {
    using get_hashes = cryptonote::rpc::GetHashesFast;

    MINFO("Starting blockchain sync with daemon");

    get_hashes::Request req{};
    req.start_height = 0;
    {
      auto reader = disk.start_read();
      if (!reader)
        return reader.error();

      auto chain = reader->get_chain_sync();
      if (!chain)
        return chain.error();

      req.known_hashes = std::move(*chain);
    }

    for (;;)
    {
      if (req.known_hashes.empty())
        return {lws::error::bad_blockchain};

      expect<void> sent{lws::error::daemon_timeout};

      epee::byte_slice msg = rpc::client::make_message("get_hashes_fast", req);
      auto start = std::chrono::steady_clock::now();

      while (!(sent = client.send(std::move(msg), std::chrono::seconds{1})))
      {
        if (!scanner::is_running())
          return {lws::error::signal_abort_process};

        if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
          return {lws::error::daemon_timeout};

        if (!sent.matches(std::errc::timed_out))
          return sent.error();
      }

      expect<get_hashes::Response> resp{lws::error::daemon_timeout};
      start = std::chrono::steady_clock::now();

      while (!(resp = client.receive<get_hashes::Response>(std::chrono::seconds{1}, MLWS_CURRENT_LOCATION)))
      {
        if (!scanner::is_running())
          return {lws::error::signal_abort_process};

        if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
          return {lws::error::daemon_timeout};

        if (!resp.matches(std::errc::timed_out))
          return resp.error();
      }

      //
      // Exit loop if it appears we have synced to top of chain
      //
      if (resp->hashes.size() <= 1 || resp->hashes.back() == req.known_hashes.front())
        return {std::move(client)};

      MONERO_CHECK(disk.sync_chain(db::block_id(resp->start_height), epee::to_span(resp->hashes)));

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

  void scanner::run(db::storage disk, rpc::context ctx, std::size_t thread_count, const epee::net_utils::ssl_verification_t webhook_verify, const bool enable_subaddresses)
  {
    thread_count = std::max(std::size_t(1), thread_count);

    rpc::client client{};
    for (;;)
    {
      const auto last = std::chrono::steady_clock::now();
      update_rates(ctx);

      std::vector<db::account_id> active;
      std::vector<lws::account> users;

      {
        MINFO("Retrieving current active account list");

        auto reader = MONERO_UNWRAP(disk.start_read());
        auto accounts = MONERO_UNWRAP(
          reader.get_accounts(db::account_status::active)
        );

        for (db::account user : accounts.make_range())
        {
          std::vector<std::pair<db::output_id, db::address_index>> receives{};
          std::vector<crypto::public_key> pubs{};
          auto receive_list = MONERO_UNWRAP(reader.get_outputs(user.id));

          const std::size_t elems = receive_list.count();
          receives.reserve(elems);
          pubs.reserve(elems);

          for (auto output = receive_list.make_iterator(); !output.is_end(); ++output)
          {
            auto id = output.get_value<MONERO_FIELD(db::output, spend_meta.id)>();
            auto subaddr = output.get_value<MONERO_FIELD(db::output, recipient)>();
            receives.emplace_back(std::move(id), std::move(subaddr));
            pubs.emplace_back(output.get_value<MONERO_FIELD(db::output, pub)>());
          }

          users.emplace_back(user, std::move(receives), std::move(pubs));
          active.insert(
            std::lower_bound(active.begin(), active.end(), user.id), user.id
          );
        }

        reader.finish_read();
      } // cleanup DB reader

      if (users.empty())
      {
        MINFO("No active accounts");
        checked_wait(account_poll_interval - (std::chrono::steady_clock::now() - last));
      }
      else
        check_loop(disk.clone(), ctx, thread_count, std::move(users), std::move(active), options{webhook_verify, enable_subaddresses});

      if (!scanner::is_running())
        return;

      if (!client)
        client = MONERO_UNWRAP(ctx.connect());

      expect<rpc::client> synced = sync(disk.clone(), std::move(client));
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
