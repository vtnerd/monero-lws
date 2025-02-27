// Copyright (c) 2018-2020, The Monero Project
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

#include <string>

#include "byte_slice.h"
#include "common/expect.h"
#include "wire/json/fwd.h"

namespace lws
{
  struct rates
  {
    double AUD;
    double BRL;
    double BTC;
    double CAD;
    double CHF;
    double CNY;
    double EUR;
    double GBP;
    double HKD;
    double INR;
    double JPY;
    double KRW;
    double MXN;
    double NOK;
    double NZD;
    double SEK;
    double SGD;
    double TRY;
    double USD;
    double RUB;
    double ZAR;
  };
  WIRE_JSON_DECLARE_OBJECT(rates);

  namespace rpc
  {
    struct crypto_compare_
    {
      static const char url[];

      expect<lws::rates> operator()(std::string&& body) const;
    };
    constexpr const crypto_compare_ crypto_compare{};
  } // rpc
} // lws
