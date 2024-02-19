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
#include "span.h"
#include "wire/error.h"
#include "wire/read.h"

namespace wire
{
  // enable writing of std::array
  template<typename T, std::size_t N>
  struct is_array<std::array<T, N>>
    : std::true_type
  {};

  // `std::array`s of `char` and `uint8_t` are not arrays
  template<std::size_t N>
  struct is_array<std::array<char, N>>
    : std::false_type
  {};
  template<std::size_t N>
  struct is_array<std::array<std::uint8_t, N>>
    : std::false_type
  {};

  template<typename R, std::size_t N>
  inline void read_bytes(R& source, std::array<char, N>& dest)
  {
    source.binary(epee::to_mut_span(dest));
  }
  template<typename R, std::size_t N>
  inline void read_bytes(R& source, std::array<std::uint8_t, N>& dest)
  {
    source.binary(epee::to_mut_span(dest));
  }

  template<typename W, std::size_t N>
  inline void write_bytes(W& dest, const std::array<char, N>& source)
  {
    source.binary(epee::to_span(source));
  }
  template<typename W, std::size_t N>
  inline void write_bytes(W& dest, const std::array<std::uint8_t, N>& source)
  {
    source.binary(epee::to_span(source));
  }

  // Read a fixed sized array
  template<typename R, typename T, std::size_t N>
  inline void read_bytes(R& source, std::array<T, N>& dest)
  {
    std::size_t count = source.start_array(0);
    const bool json = (count == 0);
    if (!json && count != dest.size())
      WIRE_DLOG_THROW(wire::error::schema::array, "Expected array of size " << dest.size());

    for (auto& elem : dest)
    {
      if (json && source.is_array_end(count))
        WIRE_DLOG_THROW(wire::error::schema::array, "Expected array of size " << dest.size());
      wire_read::bytes(source, elem);
      --count;
    }
    if (!source.is_array_end(count))
      WIRE_DLOG_THROW(wire::error::schema::array, "Expected array of size " << dest.size());
    source.end_array();
  }
}

