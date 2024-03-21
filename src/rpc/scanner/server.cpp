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

#include <boost/asio/coroutine.hpp>
#include <boost/lexical_cast.hpp>
#include <vector>
#include "byte_slice.h"  // monero/contrib/epee/include
#include "byte_stream.h" // monero/contrib/epee/include
#include "error.h"
#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "rpc/scanner/read_commands.h"
#include "rpc/scanner/write_commands.h"
#include "scanner.h"

namespace lws { namespace rpc { namespace scanner
{
  namespace
  {
    /*! \brief Only way to immediately return from io_service post/run call.
      Error handling is as follows in this file:
        - Error local to remote endpoint - use bool return
        - Recoverable blockchain or account state issue - throw `reset_state`
        - Unrecoverable error - throw hard exception (close process). */
    struct reset_state : std::exception
    {
      reset_state() :
        std::exception()
      {}

      virtual const char* what() const noexcept override
      { return "reset_state / abort scan"; }
    };

    //! \brief Handler for request to give accounts to another scanner
    struct give_accounts_handler
    {
      using input = give_accounts;
      static bool handle(server_connection& self, std::vector<db::account_id> users);
      static bool handle(const std::shared_ptr<server_connection>& self, input msg)
      { 
        if (!self)
          return false;
        return handle(*self, std::move(msg.users));
      }
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
    db::storage disk_;
    rpc::client zclient_;
    std::vector<db::account_id> accounts_; //!< Remote is scanning these accounts
    ssl_verification_t webhook_verify_;

    explicit server_connection(boost::asio::io_service& context, db::storage disk, rpc::client zclient, ssl_verification_t webhook_verify)
      : connection(context),
        disk_(std::move(disk)),
        zclient_(std::move(zclient)),
        accounts_(),
        webhook_verify_(webhook_verify)
    {
      MINFO("New scanner client at " << remote_address());
    }

    //! \return Handlers for commands from client
    static const std::array<command, 2>& commands() noexcept
    {
      static constexpr const std::array<command, 2> value{{
        call<give_accounts_handler, server_connection>,
        call<update_accounts_handler, server_connection>
      }};
      static_assert(give_accounts_handler::input::id() == 0);
      static_assert(update_accounts_handler::input::id() == 1);
      return value;
    }

    //! Pulls accounts from inproc ZMQ and pushes to remote TCP client
    static void pull_push(std::shared_ptr<server_connection> self)
    {
      if (!self)
        return;

      auto pulled = MONERO_UNWRAP(self->zclient_.pull_accounts());
      if (pulled.empty())
        return;

      for (const auto& account : pulled)
        self->accounts_.push_back(account.id());
      if (!write_command(self, push_accounts{std::move(pulled)}))
        self->cleanup();
    }
    
    //! Cancels pending operations and "pushes" accounts to other processes
    void cleanup()
    {
      base_cleanup();
      if (accounts_.empty())
        return;
      give_accounts_handler::handle(*this, std::move(accounts_));
      accounts_.clear();
    }
  };

  namespace
  {
    bool give_accounts_handler::handle(server_connection& self, std::vector<db::account_id> users)
    {
      MINFO("Giving " << users.size() << " accounts from " << self.remote_address() << " to other scanners");
      return false; 
    }

    bool update_accounts_handler::handle(const std::shared_ptr<server_connection>& self, input msg)
    {
      std::sort(msg.users.begin(), msg.users.end(), by_height{});
      auto updated = self->disk_.update(
        msg.users.front().scan_height(), epee::to_span(msg.blocks), epee::to_span(msg.users), nullptr /* untrusted mode not allowed */
      );

      if (!updated)
      {
        if (updated == lws::error::blockchain_reorg)
        {
          MINFO("Blockchain reorg detected, resetting state");
          throw reset_state{};
        }
        MONERO_THROW(updated.error(), "Failed to update accounts on disk");
      }

      MINFO("Processed (remotely at " << self->remote_address() << ") " << msg.blocks.size() << " block(s) against " << msg.users.size() << " account(s)");
      lws::scanner::send_payment_hook(self->zclient_, epee::to_span(updated->second), self->webhook_verify_);
      if (updated->first != self->accounts_.size())
      {
        MWARNING("Only updated " << updated->first << " account(s) (from " << self->remote_address() << ") out of " << self->accounts_.size() << ", resetting");
        throw reset_state{};
      }
      return true;
    }
  } // anonymous

  class server::acceptor : public boost::asio::coroutine
  {
    server* self_;
    std::shared_ptr<server_connection> next_;

  public:
    explicit acceptor(server& self)
      : boost::asio::coroutine(), self_(std::addressof(self)), next_(nullptr)
    {}

    void operator()(const boost::system::error_code& error = {})
    {
      if (!self_ || error)
      {
        if (error == boost::asio::error::operation_aborted)
          return; // exiting
        MONERO_THROW(error, "server acceptor failed");
      }
      BOOST_ASIO_CORO_REENTER(*this)
      {
        for (;;)
        {
          next_ = std::make_shared<server_connection>(
            self_->context_, self_->disk_.clone(), MONERO_UNWRAP(self_->zclient_.clone()), self_->webhook_verify_
          );
          BOOST_ASIO_CORO_YIELD self_->acceptor_.async_accept(next_->sock_, *this);

          // delay enable_pull_accounts until async_accept completes
          MONERO_UNWRAP(next_->zclient_.enable_pull_accounts());
          self_->scanners_.emplace(next_);
          read_commands<server_connection>{std::move(next_)}();
        }
      }
    }
  };

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
      boost::asio::ip::address::from_string(host), boost::lexical_cast<unsigned short>(port)
    };
  }

  server::server(const std::string& address, db::storage disk, rpc::client zclient, ssl_verification_t webhook_verify)
    : context_(),
      acceptor_(context_),
      scanners_(),
      disk_(std::move(disk)),
      zclient_(std::move(zclient)),
      webhook_verify_(webhook_verify)
  {
    
    const auto endpoint = get_endpoint(address); 
    acceptor_.open(endpoint.protocol());
    acceptor_.bind(endpoint);
    acceptor_.listen();

    acceptor{*this}();
  }

  server::~server()
  {}

  expect<void> server::poll_io()
  {
    try
    {
      // a bit inefficient, but I don't want an aggressive scanner blocking this
      for (unsigned i = 0 ; i < 1000; ++i)
      { 
        context_.reset();
        if (!context_.poll_one())
          break;
      }
    }
    catch (const reset_state&)
    {
      return {error::signal_abort_scan};
    }

    for (auto conn = scanners_.begin(); conn != scanners_.end(); )
    {
      auto shared = conn->lock();
      if (shared)
      {
        ++conn;
        server_connection::pull_push(std::move(shared));
      }
      else
        conn = scanners_.erase(conn);
    }
    return success();
  }
}}} // lws // rpc // scanner
