// Copyright (c) 2018-2019, The Monero Project
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

#pragma once

#include <boost/thread/thread.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "db/storage.h"
#include "net/net_ssl.h"
#include "rpc/client.h"
#include "span.h"

namespace lws
{
  class mempool;
  struct rest_server_data;
  class rest_server
  {
    struct internal;
    template<typename> struct connection;
    template<typename> struct handler_loop;
    template<typename> struct accept_loop;

    std::unique_ptr<rest_server_data> global_;
    std::list<internal> ports_;
    std::vector<boost::thread> workers_;

    void run_io();

  public:
    struct configuration
    {
      epee::net_utils::ssl_authentication_t auth;
      std::vector<std::string> access_controls;
      std::size_t threads;
      std::uint32_t max_subaddresses;
      epee::net_utils::ssl_verification_t webhook_verify;
      bool allow_external;
      bool disable_admin_auth;
      bool auto_accept_creation;
      bool auto_accept_import;
    };

    explicit rest_server(
      epee::span<const std::string> addresses,
      std::vector<std::string> admin,
      db::storage disk,
      rpc::client client,
      std::shared_ptr<lws::mempool> mempool,
      configuration config);

    rest_server(rest_server&&) = delete;
    rest_server(rest_server const&) = delete;

    ~rest_server() noexcept;

    rest_server& operator=(rest_server&&) = delete;
    rest_server& operator=(rest_server const&) = delete;
  };
}
