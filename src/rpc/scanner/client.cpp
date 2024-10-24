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

#include "client.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <chrono>

#include "common/expect.h"      // monero/src
#include "misc_log_ex.h"        // monero/contrib/epee/include
#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "rpc/scanner/read_commands.h"
#include "rpc/scanner/server.h"
#include "rpc/scanner/write_commands.h"

namespace lws { namespace rpc { namespace scanner
{
  namespace
  {
    //! Connection completion timeout
    constexpr const std::chrono::seconds connect_timeout{5};
    
    //! Retry connection timeout
    constexpr const std::chrono::seconds reconnect_interval{10};

    struct push_accounts_handler
    {
      using input = push_accounts;
      static bool handle(const std::shared_ptr<client>& self, input msg)
      {
        if (!self)
          return false;
        if (msg.users.empty())
          return true;
        client::push_accounts(self, std::move(msg.users));
        return true;
      }
    };

    struct replace_accounts_handler
    {
      using input = replace_accounts;
      static bool handle(const std::shared_ptr<client>& self, input msg)
      {
        if (!self)
          return false;
        // push empty accounts too, indicates we should stop scanning
        client::replace_accounts(self, std::move(msg.users));
        return true;
      }
    };
  } // anonymous

  //! \brief Closes the socket, forcing all outstanding ops to cancel.
  struct client::close
  {
    std::shared_ptr<client> self_;

    void operator()(const boost::system::error_code& error = {}) const
    {
      if (self_ && error != boost::asio::error::operation_aborted)
      {
        // The `cleanup()` function is meant to cleanup then destruct connection
        assert(self_->strand_.running_in_this_thread());
        boost::system::error_code error{};
        self_->sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
        error = boost::system::error_code{};
        self_->sock_.close(error);
        if (error)
          MERROR("Error when closing socket: " << error.message());
      }
    }
  };

  //! \brief 
  class client::connector : public boost::asio::coroutine
  {
    std::shared_ptr<client> self_;
  public:
    explicit connector(std::shared_ptr<client> self)
      : boost::asio::coroutine(), self_(std::move(self))
    {}

    void operator()(const boost::system::error_code& error = {})
    {
      if (!self_ || error == boost::asio::error::operation_aborted)
        return;

      assert(self_->strand_.running_in_this_thread());
      BOOST_ASIO_CORO_REENTER(*this)
      {
        for (;;)
        {
          MINFO("Attempting connection to " << self_->server_address_);
          self_->connect_timer_.expires_from_now(connect_timeout);
          self_->connect_timer_.async_wait(
            boost::asio::bind_executor(self_->strand_, close{self_})
          );

          BOOST_ASIO_CORO_YIELD self_->sock_.async_connect(
            self_->server_address_, boost::asio::bind_executor(self_->strand_, *this)
          );

          if (!self_->connect_timer_.cancel() || error)
          {
            MERROR("Connection attempt failed: " << error.message());
            close{self_}();
          }
          else
            break;

          MINFO("Retrying connection in " << std::chrono::seconds{reconnect_interval}.count() << " seconds"); 
          self_->connect_timer_.expires_from_now(reconnect_interval);
          BOOST_ASIO_CORO_YIELD self_->connect_timer_.async_wait(
            boost::asio::bind_executor(self_->strand_, *this)
          );
        }

        MINFO("Connection made to " << self_->server_address_);
        self_->connected_ = true;
        const auto threads = boost::numeric_cast<std::uint32_t>(self_->local_.size());
        write_command(self_, initialize{self_->pass_, threads});
        read_commands(self_);
      }
    }
  };

  client::client(boost::asio::io_context& io, const std::string& address, std::string pass, std::vector<std::shared_ptr<queue>> local)
    : connection(io),
      local_(std::move(local)),
      pass_(std::move(pass)),
      next_push_(0),
      connect_timer_(io),
      server_address_(rpc::scanner::server::get_endpoint(address)),
      connected_(false)
  {
    for (const auto& queue : local_)
    {
      if (!queue)
        MONERO_THROW(common_error::kInvalidArgument, "nullptr local queue");
    }
  }

  client::~client()
  {}

  //! \return Handlers for commands from server
  const std::array<client::command, 2>& client::commands() noexcept
  {
    static constexpr const std::array<command, 2> value{{
      call<push_accounts_handler, client>,
      call<replace_accounts_handler, client>
    }};
    static_assert(push_accounts_handler::input::id() == 0);
    static_assert(replace_accounts_handler::input::id() == 1);
    return value;
  }

  void client::connect(const std::shared_ptr<client>& self)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    boost::asio::dispatch(
      self->strand_,
      [self] ()
      {
        if (!self->sock_.is_open())
          connector{self}();
      }
    );
  }

  void client::push_accounts(const std::shared_ptr<client>& self, std::vector<lws::account> users)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");

    boost::asio::dispatch(
      self->strand_,
      [self, users = std::move(users)] () mutable
      {
        /* Keep this algorithm simple, one user at a time. A little more difficult
          to do multiples at once */
        MINFO("Adding " << users.size() << " new accounts to workload");
        for (std::size_t i = 0; i < users.size(); ++i)
        {
          self->local_[self->next_push_++]->push_accounts(
            std::make_move_iterator(users.begin() + i),
            std::make_move_iterator(users.begin() + i + 1)
          );
          self->next_push_ %= self->local_.size();
        }
      }
    );
  }

  void client::replace_accounts(const std::shared_ptr<client>& self, std::vector<lws::account> users)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");

    boost::asio::dispatch(
      self->strand_,
      [self, users = std::move(users)] () mutable
      {
        MINFO("Received " << users.size() << " accounts as new workload");
        for (std::size_t i = 0; i < self->local_.size(); ++i)
        {
          // count == 0 is OK. This will tell the thread to stop working
          const auto count = users.size() / (self->local_.size() - i);
          std::vector<lws::account> next{
            std::make_move_iterator(users.end() - count),
            std::make_move_iterator(users.end())
          };
          users.erase(users.end() - count, users.end());
          self->local_[i]->replace_accounts(std::move(next));
        }
        self->next_push_ = 0;
      }
    );
  }

  void client::send_update(const std::shared_ptr<client>& self, std::vector<lws::account> users, std::vector<crypto::hash> blocks)
  {
    if (!self)
      MONERO_THROW(common_error::kInvalidArgument, "nullptr self");
    
    boost::asio::dispatch(
      self->strand_,
      [self, users = std::move(users), blocks = std::move(blocks)] () mutable
      {
        if (!self->connected_)
          MONERO_THROW(common_error::kInvalidArgument, "not connected");
        write_command(self, update_accounts{std::move(users), std::move(blocks)});
      }
    );
  }

  void client::cleanup()
  {
    base_cleanup();
    context().stop();
  }
}}} // lws // rpc // scanner
