// Copyright (c) 2024, The Monero Project
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

#include "server.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <sodium/utils.h>
#include <sodium/randombytes.h>
#include <vector>
#include "byte_slice.h"    // monero/contrib/epee/include
#include "byte_stream.h"   // monero/contrib/epee/include
#include "common/expect.h" // monero/src/
#include "error.h"
#include "misc_log_ex.h"        // monero/contrib/epee/include
#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "rpc/scanner/read_commands.h"
#include "rpc/scanner/write_commands.h"
#include "scanner.h"

namespace lws { namespace rpc { namespace scanner
{
  namespace
  {
    //! Use remote scanning only if users-per-local-thread exceeds this
    constexpr const std::size_t remote_threshold = 100;

    //! Threshold for resetting/replacing state instead of pushing
    constexpr const std::size_t replace_threshold = 10000;

    //! \brief Handler for server to initialize new scanner
    struct initialize_handler
    {
      using input = initialize;
      static bool handle(const std::shared_ptr<server_connection>& self, input msg);
    };

    //! \brief Handler for request to update accounts
    struct update_accounts_handler
    {
      using input = update_accounts;
      static bool handle(const std::shared_ptr<server_connection>& self, input msg);
    };

    using command = bool(*)(const std::shared_ptr<server_connection>&);
  } // anonymous

  //! \brief Context/state for remote `monero-lws-scanner` instance.
  struct server_connection : connection
  {
    const std::shared_ptr<server> parent_;
    std::size_t threads_; //!< Number of scan threads at remote process

  public:
    explicit server_connection(std::shared_ptr<server> parent, boost::asio::io_context& io)
      : connection(io),
        parent_(std::move(parent)),
        threads_(0)
    {
      if (!parent_)
        MONERO_THROW(common_error::kInvalidArgument, "nullptr parent");
    }

    //! \return Handlers for commands from client
    static const std::array<command, 2>& commands() noexcept
    {
      static constexpr const std::array<command, 2> value{{
        call<initialize_handler, server_connection>,
        call<update_accounts_handler, server_connection>
      }};
      static_assert(initialize_handler::input::id() == 0);
      static_assert(update_accounts_handler::input::id() == 1);
      return value;
    }

    //! Cancels pending operations and "pushes" accounts to other processes
    void cleanup()
    {
      base_cleanup();
    }
  };

  namespace
  {
    bool initialize_handler::handle(const std::shared_ptr<server_connection>& self, const input msg)
    {
      if (!self)
        return false;

      assert(self->strand_.running_in_this_thread());
      if (self->threads_)
      {
        MERROR("Client ( " << self->remote_endpoint() << ") invoked initialize twice, closing connection");
        return false;
      }

      if (!msg.threads)
      {
        MERROR("Client (" << self->remote_endpoint() << ") intialized with 0 threads");
        return false;
      }

      if (!self->parent_->check_pass(msg.pass))
      {
        MERROR("Client (" << self->remote_endpoint() << ") provided invalid pass");
        return false;
      }

      self->threads_ = boost::numeric_cast<std::size_t>(msg.threads);
      server::replace_users(self->parent_);
      return true;
    }

    bool update_accounts_handler::handle(const std::shared_ptr<server_connection>& self, input msg)
    {
      if (!self)
        return false;

      if (msg.users.empty())
        return true;

      MINFO("Client (" << self->remote_endpoint() << ") processed "
        << msg.blocks.size() << " block(s) against " << msg.users.size() << " account(s)");
      server::store(self->parent_, std::move(msg.users), std::move(msg.blocks));
      return true;  
    }
  } // anonymous

  class server::acceptor : public boost::asio::coroutine
  {
    std::shared_ptr<server> self_;
    std::shared_ptr<server_connection> next_;

  public:
    explicit acceptor(std::shared_ptr<server> self)
      : boost::asio::coroutine(), self_(std::move(self)), next_(nullptr)
    {}

    void operator()(const boost::system::error_code& error = {})
    {
      if (error)
      {
        if (error == boost::asio::error::operation_aborted)
          return; // exiting
        MONERO_THROW(error, "server acceptor failed");
      }

      if (!self_ || self_->stop_)
        return;

      assert(self_->strand_.running_in_this_thread());
      BOOST_ASIO_CORO_REENTER(*this)
      {
        for (;;)
        {
          next_ = std::make_shared<server_connection>(self_, self_->strand_.context());
          BOOST_ASIO_CORO_YIELD self_->acceptor_.async_accept(
            next_->sock_, boost::asio::bind_executor(self_->strand_, *this)
          );

          MINFO("New connection to " << next_->remote_endpoint() << " / " << next_.get());

          self_->remote_.emplace(next_);
          read_commands(std::move(next_));
        }
      }
    }
  };

  struct server::check_users
  {
    std::shared_ptr<server> self_;

    void operator()(const boost::system::error_code& error = {}) const
    {
      if (!self_ || self_->stop_ || error == boost::asio::error::operation_aborted)
        return;

      assert(self_->strand_.running_in_this_thread());
      self_->check_timer_.expires_after(account_poll_interval);
      self_->check_timer_.async_wait(boost::asio::bind_executor(self_->strand_, *this));

      std::size_t total_threads = self_->local_.size();
      std::vector<std::shared_ptr<server_connection>> remotes{};
      remotes.reserve(self_->remote_.size());
      for (auto& remote : self_->remote_)
      {
        auto conn = remote.lock();
        if (!conn)
        {
          // connection loss detected, re-shuffle accounts
          self_->do_replace_users();
          return;
        }
        if (std::numeric_limits<std::size_t>::max() - total_threads < conn->threads_)
          MONERO_THROW(error::configuration, "Exceeded max threads (size_t) across all systems");
        total_threads += conn->threads_;
        remotes.push_back(std::move(conn));
      }

      if (!total_threads)
      {
        MWARNING("Currently no worker threads, waiting for new clients");
        return;
      }

      auto reader = self_->disk_.start_read();
      if (!reader)
      {
        if (reader.matches(std::errc::no_lock_available))
        {
          MWARNING("Failed to open DB read handle, retrying later");
          return;
        }
        MONERO_THROW(reader.error(), "Failed to open DB read handle");
      }

      auto current_users = MONERO_UNWRAP(
        reader->get_accounts(db::account_status::active, std::move(self_->accounts_cur_))
      );
      if (current_users.count() < self_->active_.size())
      {
        // a shrinking user base, re-shuffle
        reader->finish_read();
        self_->accounts_cur_ = current_users.give_cursor();
        self_->do_replace_users();
        return;
      }
      std::vector<db::account_id> active_copy = self_->active_;
      std::vector<lws::account> new_accounts;
      for (auto user = current_users.make_iterator(); !user.is_end(); ++user)
      {
        const db::account_id user_id = user.get_value<MONERO_FIELD(db::account, id)>();
        const auto loc = std::lower_bound(active_copy.begin(), active_copy.end(), user_id);
        if (loc == active_copy.end() || *loc != user_id)
        {
          new_accounts.push_back(MONERO_UNWRAP(reader->get_full_account(user.get_value<db::account>())));
          if (replace_threshold < new_accounts.size())
          {
            reader->finish_read();
            self_->accounts_cur_ = current_users.give_cursor();
            self_->do_replace_users();
            return;
          }
          self_->active_.insert(
            std::lower_bound(self_->active_.begin(), self_->active_.end(), user_id),
            user_id
          );
        }
        else
          active_copy.erase(loc);
      }

      if (!active_copy.empty())
      {
        reader->finish_read();
        self_->accounts_cur_ = current_users.give_cursor();
        self_->do_replace_users();
        return;
      }

      self_->next_thread_ %= total_threads;
      while (!new_accounts.empty())
      {
        if (self_->next_thread_ < self_->local_.size())
        {
          self_->local_[self_->next_thread_]->push_accounts(
            std::make_move_iterator(new_accounts.end() - 1),
            std::make_move_iterator(new_accounts.end())
          );
          new_accounts.erase(new_accounts.end() - 1);
          ++self_->next_thread_;
        }
        else
        {
          std::size_t j = 0;
          for (auto offset = self_->local_.size(); j < remotes.size(); ++j)
          {
            if (self_->next_thread_ <= offset)
              break;
            offset += remotes[j]->threads_;
          }

          const auto user_count = std::min(new_accounts.size(), remotes[j]->threads_);
          std::vector<lws::account> next{
            std::make_move_iterator(new_accounts.end() - user_count),
            std::make_move_iterator(new_accounts.end())
          };
          new_accounts.erase(new_accounts.end() - user_count);
          write_command(remotes[j], push_accounts{std::move(next)});
          self_->next_thread_ += remotes[j]->threads_;
        }

        self_->next_thread_ %= total_threads;
      }
      reader->finish_read();
      self_->accounts_cur_ = current_users.give_cursor();
    }
  };

  void server::do_replace_users()
  {
    assert(strand_.running_in_this_thread());
    MINFO("Updating/replacing user account(s) on worker thread(s)");

    std::size_t remaining_threads = local_.size();
    std::vector<std::shared_ptr<server_connection>> remotes;
    remotes.reserve(remote_.size());
    for (auto remote = remote_.begin(); remote != remote_.end(); )
    {
      auto conn = remote->lock();
      if (conn)
      {
        if (std::numeric_limits<std::size_t>::max() - remaining_threads < conn->threads_)
          MONERO_THROW(error::configuration, "Exceeded max threads (size_t) across all systems");

        remaining_threads += conn->threads_;
        remotes.push_back(std::move(conn));
        ++remote;
      }
      else
        remote = remote_.erase(remote);
    }

    if (!remaining_threads)
    {
      MWARNING("Currently no worker threads, waiting for new clients");
      return;
    }

    std::vector<db::account_id> active{};
    std::vector<db::account> users{};
    auto reader = MONERO_UNWRAP(disk_.start_read());
    {
      auto active_users = MONERO_UNWRAP(reader.get_accounts(db::account_status::active));
      const auto active_count = active_users.count();
      active.reserve(active_count);
      users.reserve(active_count);
      for (auto user : active_users.make_range())
      {
        active.insert(std::lower_bound(active.begin(), active.end(), user.id), user.id);
        users.insert(std::lower_bound(users.begin(), users.end(), user, by_height{}), user);
      }
    }

    // if under `remote_threshold` users per thread, use local scanning only
    if (local_.size() && (users.size() / local_.size()) < remote_threshold)
      remaining_threads = local_.size();

    // make sure to notify of zero users too!
    for (auto& local : local_)
    {
      const auto user_count = users.size() / remaining_threads;

      std::vector<lws::account> next{};
      next.reserve(user_count);

      for (std::size_t j = 0; !users.empty() && j < user_count; ++j)
      {
        next.push_back(MONERO_UNWRAP(reader.get_full_account(users.back())));
        users.erase(users.end() - 1);
      }

      local->replace_accounts(std::move(next));
      --remaining_threads;
    }

    // make sure to notify of zero users too!
    for (auto& remote : remotes)
    {
      const auto users_per_thread = users.size() / std::max(std::size_t(1), remaining_threads);
      const auto user_count = std::max(std::size_t(1), users_per_thread) * remote->threads_;

      std::vector<lws::account> next{};
      next.reserve(user_count);

      for (std::size_t j = 0; !users.empty() && j < user_count; ++j)
      {
        next.push_back(MONERO_UNWRAP(reader.get_full_account(users.back())));
        users.erase(users.end() - 1);
      }

      write_command(remote, replace_accounts{std::move(next)});
      remaining_threads -= std::min(remaining_threads, remote->threads_);
    }

    next_thread_ = 0;
    active_ = std::move(active);
  }

  void server::do_stop()
  {
    assert(strand_.running_in_this_thread());
    if (stop_)
      return;

    MDEBUG("Stopping rpc::scanner::server async operations");
    boost::system::error_code error{};
    check_timer_.cancel();
    acceptor_.cancel(error);
    acceptor_.close(error);

    for (auto& remote : remote_)
    {
      const auto conn = remote.lock();
      if (conn)
        boost::asio::dispatch(conn->strand_, [conn] () { conn->cleanup(); });
    }

    stop_ = true;
  }

  boost::asio::ip::tcp::endpoint server::get_endpoint(const std::string& address)
  {
    std::string host;
    std::string port;
    {
      const auto split = address.rfind(':');
      if (split == std::string::npos)
      {
        host = "0.0.0.0";
        port = address;
      }
      else
      {
        host = address.substr(0, split);
        port = address.substr(split + 1);
      }
    }
    return boost::asio::ip::tcp::endpoint{
      boost::asio::ip::make_address(host), boost::lexical_cast<unsigned short>(port)
    };
  }

  server::server(boost::asio::io_context& io, db::storage disk, rpc::client zclient, std::vector<std::shared_ptr<queue>> local, std::vector<db::account_id> active, std::shared_ptr<boost::asio::ssl::context> ssl)
    : strand_(io),
      check_timer_(io),
      acceptor_(io),
      remote_(),
      local_(std::move(local)),
      active_(std::move(active)),
      disk_(std::move(disk)),
      zclient_(std::move(zclient)),
      webhook_(std::move(ssl)),
      accounts_cur_{},
      next_thread_(0),
      pass_hashed_(),
      pass_salt_(),
      stop_(false)
  {
    std::sort(active_.begin(), active_.end());
    for (const auto& local : local_)
    {
      if (!local)
        MONERO_THROW(common_error::kInvalidArgument, "given nullptr local queue");
    }

    std::memset(pass_hashed_.data(), 0, pass_hashed_.size());
    randombytes_buf(pass_salt_.data(), pass_salt_.size());
  }

  server::~server() noexcept
  {}

  bool server::check_pass(const std::string& pass) const noexcept
  {
    std::array<unsigned char, 32> out;
    std::memset(out.data(), 0, out.size());
    compute_hash(out, pass);
    return sodium_memcmp(out.data(), pass_hashed_.data(), out.size()) == 0;
  }

  void server::compute_hash(std::array<unsigned char, 32>& out, const std::string& pass) const noexcept
  {
    if (out.size() < crypto_pwhash_BYTES_MIN)
      MONERO_THROW(error::crypto_failure, "Invalid output size");
    if (crypto_pwhash_BYTES_MAX < out.size())
      MONERO_THROW(error::crypto_failure, "Invalid output size");

    if (pass.size() < crypto_pwhash_PASSWD_MIN && crypto_pwhash_PASSWD_MAX < pass.size())
      MONERO_THROW(error::crypto_failure, "Invalid password size");

    if (crypto_pwhash(out.data(), out.size(), pass.data(), pass.size(), pass_salt_.data(),
          crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN, crypto_pwhash_ALG_DEFAULT) != 0)
      MONERO_THROW(error::crypto_failure, "Failed password hashing");
  }

  void server::start_acceptor(const std::shared_ptr<server>& self, const std::string& address, std::string pass)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    if (address.empty())
      return;

    auto endpoint = get_endpoint(address);
    boost::asio::dispatch(
      self->strand_,
      [self, endpoint = std::move(endpoint), pass = std::move(pass)] ()
      {
        self->acceptor_.close();
        self->acceptor_.open(endpoint.protocol());
#if !defined(_WIN32)
        self->acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
#endif
        self->acceptor_.bind(endpoint);
        self->acceptor_.listen();

        MINFO("Listening at " << endpoint << " for scanner clients");

        self->compute_hash(self->pass_hashed_, pass);
        acceptor{std::move(self)}();
      }
    );
  }

  void server::start_user_checking(const std::shared_ptr<server>& self)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    boost::asio::dispatch(self->strand_, check_users{self});
  }

  void server::replace_users(const std::shared_ptr<server>& self)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    boost::asio::dispatch(self->strand_, [self] () { self->do_replace_users(); });
  }

  void server::store(const std::shared_ptr<server>& self, std::vector<lws::account> users, std::vector<crypto::hash> blocks)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");

    std::sort(users.begin(), users.end(), by_height{});
    boost::asio::dispatch(
      self->strand_,
      [self, users = std::move(users), blocks = std::move(blocks)] ()
      {
        if (!lws::user_data::store(self->strand_.context(), self->disk_, self->zclient_, self->webhook_, epee::to_span(blocks), epee::to_span(users), nullptr))
        {
          self->do_stop();
          self->strand_.context().stop();
        }
      });
  }

  void server::stop(const std::shared_ptr<server>& self)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    boost::asio::dispatch(self->strand_, [self] () { self->do_stop(); });
  }
}}} // lws // rpc // scanner
