// Copyright (c) 2022, The Monero Project
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

#include "framework.test.h"

#include <boost/core/demangle.hpp>
#include <cstdint>
#include <limits>
#include "wire/read.h"

namespace
{
  template<typename Target>
  void test_unsigned_to_unsigned(lest::env& lest_env)
  {
    using limit = std::numeric_limits<Target>;
    static constexpr const auto max =
      std::numeric_limits<std::uintmax_t>::max();
    static_assert(limit::is_integer, "expected integer");
    static_assert(!limit::is_signed, "expected unsigned");

    SETUP("uintmax_t to " + boost::core::demangle(typeid(Target).name()))
    {
      EXPECT(Target(0) == wire::integer::cast_unsigned<Target>(std::uintmax_t(0)));
      EXPECT(limit::max() == wire::integer::cast_unsigned<Target>(std::uintmax_t(limit::max())));
      if constexpr (limit::max() < max)
      {
        EXPECT_THROWS_AS(wire::integer::cast_unsigned<Target>(std::uintmax_t(limit::max()) + 1), wire::exception);
        EXPECT_THROWS_AS(wire::integer::cast_unsigned<Target>(max), wire::exception);
      }
    }
  }

  template<typename Target>
  void test_signed_to_signed(lest::env& lest_env)
  {
    using limit = std::numeric_limits<Target>;
    static constexpr const auto min =
      std::numeric_limits<std::intmax_t>::min();
    static constexpr const auto max =
      std::numeric_limits<std::intmax_t>::max();
    static_assert(limit::is_integer, "expected integer");
    static_assert(limit::is_signed, "expected signed");

    SETUP("intmax_t to " + boost::core::demangle(typeid(Target).name()))
    {
      if constexpr (min < limit::min())
      {
        EXPECT_THROWS_AS(wire::integer::cast_signed<Target>(std::intmax_t(limit::min()) - 1), wire::exception);
        EXPECT_THROWS_AS(wire::integer::cast_signed<Target>(min), wire::exception);
      }
      EXPECT(limit::min() == wire::integer::cast_signed<Target>(std::intmax_t(limit::min())));
      EXPECT(Target(0) == wire::integer::cast_signed<Target>(std::intmax_t(0)));
      EXPECT(limit::max() == wire::integer::cast_signed<Target>(std::intmax_t(limit::max())));
      if constexpr (limit::max() < max)
      {
        EXPECT_THROWS_AS(wire::integer::cast_signed<Target>(std::intmax_t(limit::max()) + 1), wire::exception);
        EXPECT_THROWS_AS(wire::integer::cast_signed<Target>(max), wire::exception);
      }
    }
  }
}


LWS_CASE("wire::integer::cast_*")
{
  SETUP("unsigned to unsigned")
  {
    test_unsigned_to_unsigned<std::uint8_t>(lest_env);
    test_unsigned_to_unsigned<std::uint16_t>(lest_env);
    test_unsigned_to_unsigned<std::uint32_t>(lest_env);
    test_unsigned_to_unsigned<std::uint64_t>(lest_env);
    test_unsigned_to_unsigned<std::uintmax_t>(lest_env);
  }
  SETUP("signed to signed")
  {
    test_signed_to_signed<std::int8_t>(lest_env);
    test_signed_to_signed<std::int16_t>(lest_env);
    test_signed_to_signed<std::int32_t>(lest_env);
    test_signed_to_signed<std::int64_t>(lest_env);
    test_signed_to_signed<std::intmax_t>(lest_env);
  }
}
