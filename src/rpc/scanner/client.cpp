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

#include <boost/asio/coroutine.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>

#include "rpc/scanner/commands.h"
#include "rpc/scanner/connection.h"
#include "rpc/scanner/read_commands.h"
#include "rpc/scanner/server.h"

namespace lws { namespace rpc { namespace scanner
{
  namespace
  {
    constexpr const std::chrono::seconds reconnect_interval{30};

    struct push_accounts_handler
    {
      using input = push_accounts;
      static bool handle(const std::shared_ptr<client_connection>& self, input msg); 
    };

    struct take_accounts_handler
    {
      using input = take_accounts;
      static bool handle(const std::shared_ptr<client_connection>& self, input msg); 
    };

    using command = bool(*)(const std::shared_ptr<client_connection>&);
  }

  struct client_connection : connection
  {
    rpc::scanner::client& sclient_;
    boost::asio::steady_timer reconnect_timer_;

    explicit client_connection(rpc::scanner::client& sclient)
      : connection(sclient.context()),
        sclient_(sclient),
        users_(),
        reconnect_timer_(sclient.context())
    {}

    //! \return Handlers for commands from server
    static const std::array<command, 2>& commands() noexcept
    {
      static constexpr const std::array<command, 2> value{{
        call<push_accounts_handler, client_connection>,
        call<take_accounts_handler, client_connection>
      }};
      static_assert(push_accounts_handler::input::id() == 0);
      static_assert(take_accounts_handler::input::id() == 1);
      return value;
    }

    void cleanup()
    {
      base_cleanup();
      sclient_.reconnect();
    }
  };

  namespace
  {
    bool push_accounts_handler::handle(const std::shared_ptr<client_connection>& self, input msg)
    {
      return true;
    }

    bool take_accounts_handler::handle(const std::shared_ptr<client_connection>& self, input msg)
    {
      return true;
    }
    
    class connector : public boost::asio::coroutine
    {
      std::shared_ptr<client_connection> self_;
    public:
      explicit connector(std::shared_ptr<client_connection> self)
        : boost::asio::coroutine(),
          self_(std::move(self))
      {}

      void operator()(const boost::system::error_code& error = {})
      {
        if (!self_ || error == boost::asio::error::operation_aborted)
          return; // exiting

        BOOST_ASIO_CORO_REENTER(*this)
        {
          for (;;)
          {
            BOOST_ASIO_CORO_YIELD self_->sock_.async_connect(self_->sclient_.server_address(), *this);
            if (!error)
              break;
            
            self_->reconnect_timer_.expires_from_now(reconnect_interval);
            BOOST_ASIO_CORO_YIELD self_->reconnect_timer_.async_wait(*this);
          }
          MINFO("Connection made to " << self_->remote_address());
          read_commands<client_connection>{std::move(self_)};
        }
      }
    };
  }

  client::client(const std::string& address)
    : context_(),
      conn_(nullptr),
      server_address_(rpc::scanner::server::get_endpoint(address))
  {
    reconnect();
  }

  client::~client()
  {}

  void client::reconnect()
  {
    conn_ = std::make_shared<client_connection>(*this);
    connector{conn_}();
  } 
}}} // lws // rpc // scanner
