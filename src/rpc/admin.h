// Copyright (c) 2023, The Monero Project
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

#include <boost/optional/optional.hpp>
#include <string>
#include <vector>
#include "common/expect.h" // monero/src
#include "crypto/crypto.h" // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "wire/fwd.h"
#include "wire/json/fwd.h"

namespace lws
{
namespace rpc
{
  struct add_account_req
  {
    db::account_address address;
    crypto::secret_key key;
  };
  void read_bytes(wire::reader&, add_account_req&);

  //! Request object for `accept_requests` and `reject_requests` endpoints.
  struct address_requests
  {
    std::vector<db::account_address> addresses;
    db::request type;
  };
  void read_bytes(wire::reader&, address_requests&);

  struct modify_account_req
  {
    std::vector<db::account_address> addresses;
    db::account_status status;
  };
  void read_bytes(wire::reader&, modify_account_req&);

  struct rescan_req
  {
    std::vector<db::account_address> addresses;
    db::block_id height;
  };
  void read_bytes(wire::reader&, rescan_req&);

  struct webhook_add_req
  {
    std::string url;
    boost::optional<std::string> token;
    boost::optional<db::account_address> address;
    boost::optional<crypto::hash8> payment_id;
    boost::optional<std::uint32_t> confirmations;
    db::webhook_type type;
  };
  void read_bytes(wire::reader&, webhook_add_req&);

  struct webhook_delete_req
  {
    std::vector<db::account_address> addresses;
  };
  void read_bytes(wire::reader&, webhook_delete_req&);

  struct webhook_delete_uuid_req
  {
    std::vector<boost::uuids::uuid> event_ids;
  };
  void read_bytes(wire::reader&, webhook_delete_uuid_req&);


  struct accept_requests_
  {
    using request = address_requests;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const accept_requests_ accept_requests{};

  struct add_account_
  {
    using request = add_account_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const add_account_ add_account{};

  struct list_accounts_
  {
    using request = expect<void>;
    expect<void> operator()(wire::json_writer& dest, db::storage disk) const;
    expect<void> operator()(wire::json_writer& dest, db::storage disk, const request&) const
    { return (*this)(dest, std::move(disk)); }
  };
  constexpr const list_accounts_ list_accounts{};

  struct list_requests_
  {
    using request = expect<void>;
    expect<void> operator()(wire::json_writer& dest, db::storage disk) const;
    expect<void> operator()(wire::json_writer& dest, db::storage disk, const request&) const
    { return (*this)(dest, std::move(disk)); }
  };
  constexpr const list_requests_ list_requests{};

  struct modify_account_
  {
    using request = modify_account_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const modify_account_ modify_account{};

  struct reject_requests_
  {
    using request = address_requests;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const reject_requests_ reject_requests{};

  struct rescan_
  {
    using request = rescan_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const rescan_ rescan{};

  struct webhook_add_
  {
    using request = webhook_add_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, request&& req) const;
  };
  constexpr const webhook_add_ webhook_add{};

  struct webhook_delete_
  {
    using request = webhook_delete_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request& req) const;
  };
  constexpr const webhook_delete_ webhook_delete{};

  struct webhook_del_uuid_
  {
    using request = webhook_delete_uuid_req;
    expect<void> operator()(wire::writer& dest, db::storage disk, request req) const;
  };
  constexpr const webhook_del_uuid_ webhook_delete_uuid{};


  struct webhook_list_
  {
    using request = expect<void>;
    expect<void> operator()(wire::writer& dest, db::storage disk) const;
    expect<void> operator()(wire::writer& dest, db::storage disk, const request&) const
    { return (*this)(dest, std::move(disk)); }
  };
  constexpr const webhook_list_ webhook_list{};

}} // lws // rpc
