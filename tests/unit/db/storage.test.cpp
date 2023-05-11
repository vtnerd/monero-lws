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


#include "storage.test.h"

#include <boost/filesystem/operations.hpp>
#include "common/util.h"   // monero/src/

namespace lws { namespace db { namespace test
{
  namespace
  {
    boost::filesystem::path get_db_location()
    {
      return tools::get_default_data_dir() + "light_wallet_server_unit_testing";
    }
  }

  cleanup_db::~cleanup_db()
  {
    boost::filesystem::remove_all(get_db_location());
  }

  storage get_fresh_db()
  {
    const boost::filesystem::path location = get_db_location();
    boost::filesystem::remove_all(location);
    boost::filesystem::create_directories(location);
    return storage::open(location.c_str(), 5);
  }

  db::account make_db_account(const account_address& pubs, const crypto::secret_key& key)
  {
    view_key converted_key{};
    std::memcpy(std::addressof(converted_key), std::addressof(unwrap(unwrap(key))), sizeof(key));
    return {
      account_id(1), account_time(0), pubs, converted_key
    };
  }

  lws::account make_account(const account_address& pubs, const crypto::secret_key& key)
  {
    return lws::account{make_db_account(pubs, key), {}, {}};
  }
}}} // lws // db // test
