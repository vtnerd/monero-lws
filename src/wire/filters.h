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

#include <cassert>
#include <type_traits>

#include "lmdb/util.h"

// These functions are to be used with `wire::as_object(...)` key filtering

namespace wire
{
  //! Callable that returns the value unchanged; default filter for `as_array` and `as_object`.
  struct identity_
  {
    template<typename T>
    const T& operator()(const T& value) const noexcept
    {
      return value;
    }
  };
  constexpr const identity_ identity{};

  //! Callable that forwards enum to get_string.
  struct enum_as_string_
  {
    template<typename T>
    auto operator()(const T value) const noexcept(noexcept(get_string(value))) -> decltype(get_string(value))
    {
      return get_string(value);
    }
  };
  constexpr const enum_as_string_ enum_as_string{};

  //! Callable that converts C++11 enum class or integer to integer value.
  struct as_integer_
  {
    template<typename T>
    lmdb::native_type<T> operator()(const T value) const noexcept
    {
      using native = lmdb::native_type<T>;
      static_assert(!std::is_signed<native>::value, "integer cannot be signed");
      return native(value);
    }
  };
  constexpr const as_integer_ as_integer{};
}
