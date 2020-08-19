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

#include <algorithm>
#include <boost/utility/string_ref.hpp>
#include <iterator>
#include <type_traits>

#include "common/expect.h" // monero/src
#include "wire/error.h"
#include "wire/read.h"
#include "wire/write.h"

#define WIRE_DEFINE_ENUM(type_, map)                                    \
  static_assert(std::is_enum<type_>::value, "get_string will fail");    \
  static_assert(!std::is_signed<type_>::value, "write_bytes will fail"); \
  const char* get_string(const type_ source) noexcept                   \
  {                                                                     \
    using native_type = std::underlying_type<type_>::type;              \
    const native_type value = native_type(source);                      \
    if (value < std::end(map) - std::begin(map))                        \
      return map[value];                                                \
    return "invalid enum value";                                        \
  }                                                                     \
  expect<type_> type_ ## _from_string(const boost::string_ref source) noexcept \
  {                                                                     \
    if (const auto elem = std::find(std::begin(map), std::end(map), source)) \
    {                                                                   \
      if (elem != std::end(map))                                        \
        return type_(elem - std::begin(map));                           \
    }                                                                   \
    return {::wire::error::schema::enumeration};                        \
  }                                                                     \
  void read_bytes(::wire::reader& source, type_& dest)                  \
  {                                                                     \
    dest = type_(source.enumeration(map));                              \
  }                                                                     \
  void write_bytes(::wire::writer& dest, const type_ source)            \
  {                                                                     \
    dest.enumeration(std::size_t(source), map);                         \
  }

#define WIRE_DEFINE_OBJECT(type, map)                          \
  void read_bytes(::wire::reader& source, type& dest)          \
  {                                                            \
    map(source, dest);                                         \
  }                                                            \
  void write_bytes(::wire::writer& dest, const type& source)   \
  {                                                            \
    map(dest, source);                                         \
  }
