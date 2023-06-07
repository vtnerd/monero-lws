// Copyright (c) 2020, The Monero Project
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

#include <type_traits>

#include "crypto/crypto.h"   // monero/src
#include "span.h"            // monero/contrib/include
#include "ringct/rctTypes.h" // monero/src
#include "wire/traits.h"

namespace crypto
{
  template<typename R>
  void read_bytes(R& source, crypto::secret_key& self)
  {
    source.binary(epee::as_mut_byte_span(unwrap(unwrap(self))));
  }
}

namespace wire
{
  WIRE_DECLARE_BLOB(crypto::ec_scalar);
  WIRE_DECLARE_BLOB(crypto::hash);
  WIRE_DECLARE_BLOB(crypto::hash8);
  WIRE_DECLARE_BLOB(crypto::key_derivation);
  WIRE_DECLARE_BLOB(crypto::key_image);
  WIRE_DECLARE_BLOB(crypto::public_key);
  WIRE_DECLARE_BLOB(crypto::signature);
  WIRE_DECLARE_BLOB(crypto::view_tag);
  WIRE_DECLARE_BLOB(rct::key);
}
