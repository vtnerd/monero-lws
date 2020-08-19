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
#include "rates.h"

#include "wire/json.h"

namespace
{
  template<typename F, typename T>
  void map_rates(F& format, T& self)
  {
    wire::object(format,
      WIRE_FIELD(AUD),
      WIRE_FIELD(BRL),
      WIRE_FIELD(BTC),
      WIRE_FIELD(CAD),
      WIRE_FIELD(CHF),
      WIRE_FIELD(CNY),
      WIRE_FIELD(EUR),
      WIRE_FIELD(GBP),
      WIRE_FIELD(HKD),
      WIRE_FIELD(INR),
      WIRE_FIELD(JPY),
      WIRE_FIELD(KRW),
      WIRE_FIELD(MXN),
      WIRE_FIELD(NOK),
      WIRE_FIELD(NZD),
      WIRE_FIELD(SEK),
      WIRE_FIELD(SGD),
      WIRE_FIELD(TRY),
      WIRE_FIELD(USD),
      WIRE_FIELD(RUB),
      WIRE_FIELD(ZAR)
    );
  }
}

namespace lws
{
  WIRE_JSON_DEFINE_OBJECT(rates, map_rates);

  namespace rpc
  {
    const char crypto_compare_::host[] = "https://min-api.cryptocompare.com:443";
    const char crypto_compare_::path[] =
      "/data/price?fsym=XMR&tsyms=AUD,BRL,BTC,CAD,CHF,CNY,EUR,GBP,"
      "HKD,INR,JPY,KRW,MXN,NOK,NZD,SEK,SGD,TRY,USD,RUB,ZAR";

    expect<lws::rates> crypto_compare_::operator()(std::string&& body) const
    {
      return wire::json::from_bytes<lws::rates>(std::move(body));
    }
  } // rpc
} // lws
