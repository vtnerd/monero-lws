// Copyright (c) 2022-2023, The Monero Project
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
#include <string>
#include <vector>
#include "wire.h"
#include "wire/json.h"
#include "wire/msgpack.h"
#include "wire/vector.h"

#include "wire/base.test.h"

namespace
{
  template<typename T>
  using limit = std::numeric_limits<T>;

  struct inner
  {
    std::uint32_t left;
    std::uint32_t right;
  };

  template<typename F, typename T>
  void inner_map(F& format, T& self)
  {
    wire::object(format, WIRE_FIELD(left), WIRE_FIELD(right));
  }
  WIRE_DEFINE_OBJECT(inner, inner_map)

  struct complex
  {
    std::vector<inner> objects;
    std::vector<std::int16_t> ints;
    std::vector<std::uint64_t> uints;
    std::vector<lws_test::small_blob> blobs;
    std::vector<std::string> strings;
    bool choice;
  };

  template<typename F, typename T>
  void complex_map(F& format, T& self)
  {
    wire::object(format,
      WIRE_FIELD(objects),
      WIRE_FIELD(ints),
      WIRE_FIELD(uints),
      WIRE_FIELD(blobs),
      WIRE_FIELD(strings),
      WIRE_FIELD(choice)
    );
  }
  WIRE_DEFINE_OBJECT(complex, complex_map)

  void verify_initial(lest::env& lest_env, const complex& self)
  {
    EXPECT(self.objects.empty());
    EXPECT(self.ints.empty());
    EXPECT(self.uints.empty());
    EXPECT(self.blobs.empty());
    EXPECT(self.strings.empty());
    EXPECT(self.choice == false);
  }

  void fill(complex& self)
  {
    self.objects = std::vector<inner>{inner{0, limit<std::uint32_t>::max()}, inner{100, 200}, inner{44444, 83434}};
    self.ints = std::vector<std::int16_t>{limit<std::int16_t>::min(), limit<std::int16_t>::max(), -3, 31234};
    self.uints = std::vector<std::uint64_t>{0, limit<std::uint64_t>::max(), 34234234, 33};
    self.blobs = {lws_test::blob_test1, lws_test::blob_test2, lws_test::blob_test3};
    self.strings = {"string1", "string2", "string3", "string4"};
    self.choice = true;
  }

  void verify_filled(lest::env& lest_env, const complex& self)
  {
    EXPECT(self.objects.size() == 3);
    EXPECT(self.objects.at(0).left == 0);
    EXPECT(self.objects.at(0).right == limit<std::uint32_t>::max());
    EXPECT(self.objects.at(1).left == 100);
    EXPECT(self.objects.at(1).right == 200);
    EXPECT(self.objects.at(2).left == 44444);
    EXPECT(self.objects.at(2).right == 83434);

    EXPECT(self.ints.size() == 4);
    EXPECT(self.ints.at(0) == limit<std::int16_t>::min());
    EXPECT(self.ints.at(1) == limit<std::int16_t>::max());
    EXPECT(self.ints.at(2) == -3);
    EXPECT(self.ints.at(3) == 31234);

    EXPECT(self.uints.size() == 4);
    EXPECT(self.uints.at(0) == 0);
    EXPECT(self.uints.at(1) == limit<std::uint64_t>::max());
    EXPECT(self.uints.at(2) == 34234234);
    EXPECT(self.uints.at(3) == 33);

    EXPECT(self.blobs.size() == 3);
    EXPECT(self.blobs.at(0) == lws_test::blob_test1);
    EXPECT(self.blobs.at(1) == lws_test::blob_test2);
    EXPECT(self.blobs.at(2) == lws_test::blob_test3);

    EXPECT(self.strings.size() == 4);
    EXPECT(self.strings.at(0) == "string1");
    EXPECT(self.strings.at(1) == "string2");
    EXPECT(self.strings.at(2) == "string3");
    EXPECT(self.strings.at(3) == "string4");

    EXPECT(self.choice == true);
  }

  template<typename T, typename U>
  void run_complex(lest::env& lest_env)
  {
    SETUP("Complex test for " + boost::core::demangle(typeid(T).name()))
    {
      complex base{};
      verify_initial(lest_env, base);

      {
        epee::byte_slice bytes{};
        EXPECT(!T::to_bytes(bytes, base));

        complex derived{};
        EXPECT(!T::template from_bytes<complex>(U{std::string{bytes.begin(), bytes.end()}}, derived));
        verify_initial(lest_env, derived);
      }

      fill(base);

      {
        epee::byte_slice bytes{};
        EXPECT(!T::to_bytes(bytes, base));

        complex derived{};
        EXPECT(!T::template from_bytes<complex>(U{std::string{bytes.begin(), bytes.end()}}, derived));
        verify_filled(lest_env, derived);
      }
    }
  }

  struct big { std::int64_t value; };
  struct small { std::int32_t value; };

  template<typename F, typename T>
  void big_map(F& format, T& self)
  { wire::object(format, WIRE_FIELD(value)); }

  template<typename F, typename T>
  void small_map(F& format, T& self)
  { wire::object(format, WIRE_FIELD(value)); }

  WIRE_DEFINE_OBJECT(big, big_map)
  WIRE_DEFINE_OBJECT(small, small_map)

  template<typename T, typename U>
  expect<small> round_trip(lest::env& lest_env, std::int64_t value)
  {
    small out{0};
    SETUP("Testing round-trip with " + std::to_string(value))
    {
      epee::byte_slice bytes{};
      EXPECT(!T::template to_bytes(bytes, big{value}));
      const std::error_code error =
        T::template from_bytes(U{std::string{bytes.begin(), bytes.end()}}, out);
      if (error)
        return error;
    }
    return out;
  }

  template<typename T, typename U>
  void not_overflow(lest::env& lest_env, std::int64_t value)
  {
    const expect<small> result = round_trip<T, U>(lest_env, value);
    EXPECT(result);
    EXPECT(result->value == value);
  }

  template<typename T, typename U>
  void overflow(lest::env& lest_env, std::int64_t value, const std::error_code error)
  {
    const expect<small> result = round_trip<T, U>(lest_env, value);
    EXPECT(result == error);
  }

  template<typename T, typename U>
  void run_overflow(lest::env& lest_env)
  {
    SETUP("Overflow test for " + boost::core::demangle(typeid(T).name()))
    {
      not_overflow<T, U>(lest_env, limit<std::int32_t>::min());
      not_overflow<T, U>(lest_env, 0);
      not_overflow<T, U>(lest_env, limit<std::int32_t>::max());

      overflow<T, U>(lest_env, std::int64_t(limit<std::int32_t>::min()) - 1, wire::error::schema::larger_integer);
      overflow<T, U>(lest_env, std::int64_t(limit<std::int32_t>::max()) + 1, wire::error::schema::smaller_integer);
    }
  }

  struct simple { bool choice; };
  static void read_bytes(wire::reader& source, simple& self)
  {
    wire::object(source, WIRE_FIELD(choice));
  }

  template<typename T, typename U>
  void run_skip(lest::env& lest_env)
  {
    complex base{};
    verify_initial(lest_env, base);
    fill(base);

    epee::byte_slice bytes{};
    EXPECT(!T::to_bytes(bytes, base));

    simple derived{};
    EXPECT(!T::template from_bytes<simple>(U{std::string{bytes.begin(), bytes.end()}}, derived));
    EXPECT(derived.choice);
  }
}

LWS_CASE("wire::reader and wire::writer complex")
{
  run_complex<wire::json, std::string>(lest_env);
  run_complex<wire::msgpack, epee::byte_slice>(lest_env);
}

LWS_CASE("wire::reader and wire::writer overflow")
{
  run_overflow<wire::json, std::string>(lest_env);
  run_overflow<wire::msgpack, epee::byte_slice>(lest_env);
}

LWS_CASE("wire::reader and wire::writer skip")
{
  run_skip<wire::json, std::string>(lest_env);
  run_skip<wire::msgpack, epee::byte_slice>(lest_env);
}

