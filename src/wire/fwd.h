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

#include <boost/utility/string_ref.hpp>
#include <type_traits>

#include "common/expect.h" // monero/src

//! Declare an enum to be serialized as an integer
#define WIRE_AS_INTEGER(type_)						\
  static_assert(std::is_enum<type_>(), "AS_INTEGER only enum types");	\
  template<typename R>							\
  inline void read_bytes(R& source, type_& dest)                        \
  {									\
    std::underlying_type<type_>::type temp{};                           \
    read_bytes(source, temp);                                           \
    dest = type_(temp);                                                 \
  }									\
  template<typename W>							\
  inline void write_bytes(W& dest, const type_ source)			\
  {                                                                     \
    write_bytes(dest, std::underlying_type<type_>::type(source));       \
  }

//! Declare an enum to be serialized as a string (json) or integer (msgpack)
#define WIRE_DECLARE_ENUM(type)                                         \
  const char* get_string(type) noexcept;                                \
  expect<type> type ## _from_string(const boost::string_ref) noexcept; \
  void read_bytes(::wire::reader&, type&);                              \
  void write_bytes(::wire::writer&, type)

//! Declare a class/struct serialization for all available formats
#define WIRE_DECLARE_OBJECT(type)                     \
  void read_bytes(::wire::reader&, type&);            \
  void write_bytes(::wire::writer&, const type&)

namespace wire
{
  class reader;
  struct writer;
}

