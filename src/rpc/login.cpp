// Copyright (c) 2026, The Monero Project
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

#include "login.h"

#include "crypto/crypto.h" // monero/src
#include "db/data.h"
#include "db/storage.h"
#include "error.h"
#include "rpc/light_wallet.h"

namespace lws { namespace rpc
{
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
  expect<std::pair<db::account, db::storage_reader>> open_account(const account_credentials& creds, const db::storage& disk)
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

}} // lws // rpc
