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
#include "framework.test.h"

#include <boost/core/demangle.hpp>
#include <boost/range/algorithm/equal.hpp>
#include <cstdint>
#include <type_traits>
#include "wire/traits.h"
#include "wire/msgpack.h"
#include "wire/vector.h"

#include "wire/base.test.h"

namespace
{
  constexpr const char basic_string[] = u8"my_string_data";
  //constexpr const char basic_[] =
  //  u8"{\"utf8\":\"my_string_data\",\"vec\":[0,127],\"data\":\"00ff2211\",\"choice\":true}";
  constexpr const std::uint8_t basic_msgpack[] = {
    0x84, 0xa4, 0x75, 0x74, 0x66, 0x38, 0xae, 0x6d, 0x79, 0x5f, 0x73, 0x74,
    0x72, 0x69, 0x6e, 0x67, 0x5f, 0x64, 0x61, 0x74, 0x61, 0xa3, 0x76, 0x65,
    0x63, 0x92, 0x00, 0x7f, 0xa4, 0x64, 0x61, 0x74, 0x61, 0xc4, 0x04, 0x00,
    0xff, 0x22, 0x11, 0xa6, 0x63, 0x68, 0x6f, 0x69, 0x63, 0x65, 0xc3
  };
  constexpr const std::uint8_t advanced_msgpack[] = {
    0x84, 0x00, 0xae, 0x6d, 0x79, 0x5f, 0x73, 0x74, 0x72, 0x69, 0x6e, 0x67,
    0x5f, 0x64, 0x61, 0x74, 0x61, 0x01, 0x92, 0x00, 0x7f, 0x02, 0xc4, 0x04,
    0x00, 0xff, 0x22, 0x11, 0xcc, 0xfe, 0xc3
  };

  template<typename T>
  struct basic_object
  {
    std::string utf8;
    std::vector<T> vec;
    lws_test::small_blob data;
    bool choice;
  };

  template<typename F, typename T>
  void basic_object_map(F& format, T& self)
  {
    wire::object(format,
      WIRE_FIELD_ID(0, utf8),
      WIRE_FIELD_ID(1, vec),
      WIRE_FIELD_ID(2, data),
      WIRE_FIELD_ID(254, choice)
    );
  }

  template<typename T>
  void read_bytes(wire::msgpack_reader& source, basic_object<T>& dest)
  { basic_object_map(source, dest); }

  template<typename T>
  void write_bytes(wire::msgpack_writer& dest, const basic_object<T>& source)
  { basic_object_map(dest, source); }

  template<typename T>
  void test_basic_reading(lest::env& lest_env)
  {
    SETUP("Basic (string keys) with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      basic_object<T> result{};
      const std::error_code error =
        wire::msgpack::from_bytes(epee::byte_slice{{basic_msgpack}}, result);
      EXPECT(!error);
      EXPECT(result.utf8 == basic_string);
      {
        const std::vector<T> expected{0, 127};
        EXPECT(result.vec == expected);
      }
      EXPECT(result.data == lws_test::blob_test1);
      EXPECT(result.choice);
    }
  }

  template<typename T>
  void test_advanced_reading(lest::env& lest_env)
  {
    SETUP("Advanced (integer keys) with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      basic_object<T> result{};
      const std::error_code error =
        wire::msgpack::from_bytes(epee::byte_slice{{advanced_msgpack}}, result);
      EXPECT(!error);
      EXPECT(result.utf8 == basic_string);
      {
        const std::vector<T> expected{0, 127};
        EXPECT(result.vec == expected);
      }
      EXPECT(result.data == lws_test::blob_test1);
      EXPECT(result.choice);
    }
  }

  template<typename T>
  void test_basic_writing(lest::env& lest_env)
  {
    SETUP("Basic (string keys) with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      const basic_object<T> val{basic_string, std::vector<T>{0, 127}, lws_test::blob_test1, true};
      epee::byte_slice result{};
      const std::error_code error = wire::msgpack::to_bytes(result, val);
      EXPECT(!error);
      EXPECT(boost::range::equal(result, epee::byte_slice{{basic_msgpack}}));
    }
  }

  template<typename T>
  void test_advanced_writing(lest::env& lest_env)
  {
    SETUP("Advanced (integer keys) with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      const basic_object<T> val{basic_string, std::vector<T>{0, 127}, lws_test::blob_test1, true};
      epee::byte_slice result{};
      {
        wire::msgpack_slice_writer out{true};
        wire_write::bytes(out, val);
        result = epee::byte_slice{out.take_sink()};
      }
      EXPECT(boost::range::equal(result, epee::byte_slice{{advanced_msgpack}}));
    }
  }
}

LWS_CASE("wire::msgpack_reader")
{
  test_basic_reading<std::int16_t>(lest_env);
  test_basic_reading<std::int32_t>(lest_env);
  test_basic_reading<std::int64_t>(lest_env);
  test_basic_reading<std::intmax_t>(lest_env);
  test_basic_reading<std::uint16_t>(lest_env);
  test_basic_reading<std::uint32_t>(lest_env);
  test_basic_reading<std::uint64_t>(lest_env);
  test_basic_reading<std::uintmax_t>(lest_env);
  
  test_advanced_reading<std::int16_t>(lest_env);
  test_advanced_reading<std::int32_t>(lest_env);
  test_advanced_reading<std::int64_t>(lest_env);
  test_advanced_reading<std::intmax_t>(lest_env);
  test_advanced_reading<std::uint16_t>(lest_env);
  test_advanced_reading<std::uint32_t>(lest_env);
  test_advanced_reading<std::uint64_t>(lest_env);
  test_advanced_reading<std::uintmax_t>(lest_env);
}

LWS_CASE("wire::msgpack_writer")
{
  test_basic_writing<std::int16_t>(lest_env);
  test_basic_writing<std::int32_t>(lest_env);
  test_basic_writing<std::int64_t>(lest_env);
  test_basic_writing<std::intmax_t>(lest_env);
  test_basic_writing<std::uint16_t>(lest_env);
  test_basic_writing<std::uint32_t>(lest_env);
  test_basic_writing<std::uint64_t>(lest_env);
  test_basic_writing<std::uintmax_t>(lest_env);
  
  test_advanced_writing<std::int16_t>(lest_env);
  test_advanced_writing<std::int32_t>(lest_env);
  test_advanced_writing<std::int64_t>(lest_env);
  test_advanced_writing<std::intmax_t>(lest_env);
  test_advanced_writing<std::uint16_t>(lest_env);
  test_advanced_writing<std::uint32_t>(lest_env);
  test_advanced_writing<std::uint64_t>(lest_env);
  test_advanced_writing<std::uintmax_t>(lest_env);
}
