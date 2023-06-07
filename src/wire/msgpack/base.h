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

#include <cstdint>
#include <limits>
#include <string>
#include <limits>
#include <tuple>

#include "byte_slice.h"
#include "common/expect.h"
#include "wire/msgpack/fwd.h"
#include "wire/read.h"
#include "wire/write.h"

namespace wire
{
  struct msgpack
  {
    using input_type = msgpack_reader;
    using output_type = msgpack_slice_writer;

    //! Tags that do not require bitmask to identify
    enum class tag : std::uint8_t
    {
      nil = 0xc0,
      unused,
      False,
      True,
      binary8,
      binary16,
      binary32,
      extension8,
      extension16,
      extension32,
      float32,
      float64,
      uint8,
      uint16,
      uint32,
      uint64,
      int8,
      int16,
      int32,
      int64,
      fixed_extension1,
      fixed_extension2,
      fixed_extension4,
      fixed_extension8,
      fixed_extension16,
      string8,
      string16,
      string32,
      array16,
      array32,
      object16,
      object32
    };

    //! Link a fixed tag `T` to its corresponding mask `M` and max value `N`
    template<std::uint8_t T, std::uint8_t M, std::uint8_t N>
    struct fixed_tag
    {
      static constexpr std::uint8_t tag() noexcept { return T; }
      static constexpr std::uint8_t mask() noexcept { return M; }
      static constexpr std::uint8_t max() noexcept { return N; }

      //! \return True if `value` is fixed tag `T`
      static constexpr bool matches(const std::uint8_t value) noexcept
      { return (value & mask()) == tag(); }

      //! \return True if `value` is fixed tag `T`
      static constexpr bool matches(const msgpack::tag value) noexcept
      { return matches(std::uint8_t(value)); }

      //! \return Value encoded in fixed tag
      static constexpr std::uint8_t extract(const std::uint8_t value) noexcept
      { return value & ~mask(); }

      //! \return Value encoded in fixed tag
      static constexpr std::uint8_t extract(const msgpack::tag value) noexcept
      { return extract(std::uint8_t(value)); }
    };

    // Tags requiring bitmask to identify
    using ftag_unsigned = fixed_tag<0x00, 0x80, 0x7f>;
    using ftag_signed =   fixed_tag<0xe0, 0xe0, 0>;
    using ftag_string =   fixed_tag<0xa0, 0xe0, 31>;
    using ftag_array =    fixed_tag<0x90, 0xf0, 15>;
    using ftag_object =   fixed_tag<0x80, 0xf0, 15>;

    //! Link a msgpack tag to a C++ numeric
    template<typename T, tag V>
    struct type
    {
      static constexpr bool is_signed() noexcept { return std::numeric_limits<T>::is_signed; }
      static constexpr T min() noexcept { return std::numeric_limits<T>::min(); }
      static constexpr T max() noexcept { return std::numeric_limits<T>::max(); }
      static constexpr tag Tag() noexcept { return V; }
    };

    using int8 = type<std::int8_t,   tag::int8>;
    using int16 = type<std::int16_t, tag::int16>;
    using int32 = type<std::int32_t, tag::int32>;
    using int64 = type<std::int64_t, tag::int64>;
    using signed_types = std::tuple<int8, int16, int32, int64>;

    using uint8 = type<std::uint8_t,   tag::uint8>;
    using uint16 = type<std::uint16_t, tag::uint16>;
    using uint32 = type<std::uint32_t, tag::uint32>;
    using uint64 = type<std::uint64_t, tag::uint64>;
    using unsigned_types = std::tuple<uint8, uint16, uint32, uint64>;

    using integer_types = std::tuple<
      msgpack::uint8, msgpack::int8, msgpack::uint16, msgpack::int16,
      msgpack::uint32, msgpack::int32, msgpack::uint64, msgpack::int64
    >;

    using string8 = type<std::uint8_t,   tag::string8>;
    using string16 = type<std::uint16_t, tag::string16>;
    using string32 = type<std::uint32_t, tag::string32>;
    using string_types = std::tuple<string8, string16, string32>;

    using binary8 = type<std::uint8_t,   tag::binary8>;
    using binary16 = type<std::uint16_t, tag::binary16>;
    using binary32 = type<std::uint32_t, tag::binary32>;
    using binary_types = std::tuple<binary8, binary16, binary32>;

    using extension8 = type<std::uint8_t,   tag::extension8>;
    using extension16 = type<std::uint16_t, tag::extension16>;
    using extension32 = type<std::uint32_t, tag::extension32>;
    using extension_types = std::tuple<extension8, extension16, extension32>;

    using array16 = type<std::uint16_t, tag::array16>;
    using array32 = type<std::uint32_t, tag::array32>;
    using array_types = std::tuple<array16, array32>;

    using object16 = type<std::uint16_t, tag::object16>;
    using object32 = type<std::uint32_t, tag::object32>;
    using object_types = std::tuple<object16, object32>;
    
    template<typename T>
    static std::error_code from_bytes(epee::byte_slice&& source, T& dest)
    {
      return wire_read::from_bytes<input_type>(std::move(source), dest);
    }

    template<typename T, typename U>
    static std::error_code to_bytes(T& dest, const U& source)
    {
      return wire_write::to_bytes<output_type>(dest, source);
    }
  };
}

