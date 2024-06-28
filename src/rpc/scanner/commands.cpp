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

#include "commands.h"

#include "db/account.h"
#include "db/data.h"
#include "wire/adapted/crypto.h"
#include "wire/vector.h"
#include "wire/msgpack.h"
#include "wire/wrapper/trusted_array.h"

namespace lws { namespace rpc { namespace scanner
{
  namespace
  {
    template<typename F, typename T>
    void map_initialize(F& format, T& self)
    {
      wire::object(format, WIRE_FIELD_ID(0, pass), WIRE_FIELD_ID(1, threads));
    }
  }
  WIRE_MSGPACK_DEFINE_OBJECT(initialize, map_initialize);

  namespace
  {
    template<typename F, typename T>
    void map_account_update(F& format, T& self)
    {
      wire::object(format,
        wire::optional_field<0>("users", wire::trusted_array(std::ref(self.users))),
        wire::optional_field<1>("blocks", wire::trusted_array(std::ref(self.blocks)))
      );
    }
  } 
  WIRE_MSGPACK_DEFINE_OBJECT(update_accounts, map_account_update)

  namespace
  {
    template<typename F, typename T>
    void map_push_accounts(F& format, T& self)
    {
      wire::object(format,
        wire::optional_field<0>("users", wire::trusted_array(std::ref(self.users)))
      );
    }
  }
  WIRE_MSGPACK_DEFINE_OBJECT(push_accounts, map_push_accounts);

  namespace
  {
    template<typename F, typename T>
    void map_replace_accounts(F& format, T& self)
    {
      wire::object(format,
        wire::optional_field<0>("users", wire::trusted_array(std::ref(self.users)))
      );
    }
  }
  WIRE_MSGPACK_DEFINE_OBJECT(replace_accounts, map_replace_accounts);
}}} // lws // rpc // scanner
