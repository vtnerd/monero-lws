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

#include <type_traits>
#include <utility>

#define WIRE_DECLARE_BLOB(type)                 \
  template<>                                    \
  struct is_blob<type>                          \
    : std::true_type                            \
  {}

#define WIRE_DECLARE_BLOB_NS(type)              \
  namespace wire { WIRE_DECLARE_BLOB(type); }

namespace wire
{
  template<typename T>
  struct unwrap_reference
  {
    using type = std::remove_cv_t<std::remove_reference_t<T>>;
  };

  template<typename T>
  struct unwrap_reference<std::reference_wrapper<T>>
    : std::remove_cv<T>
  {};

  template<typename T>
  using unwrap_reference_t = typename unwrap_reference<T>::type;

  /*! Mark `T` as an array for writing, and reading when
   `default_min_element_size<T::value_type>::value != 0`. See `array_` in
   `wrapper/array.h`. */
  template<typename T>
  struct is_array : std::false_type
  {};

  /*! Mark `T` as fixed binary data for reading+writing. Concept requirements
    for reading:
      * `T` must be compatible with `epee::as_mut_byte_span` (`std::is_pod<T>`
        and no padding).
    Concept requirements for writing:
      * `T` must be compatible with `epee::as_byte_span` (std::is_pod<T>` and
        no padding). */
  template<typename T>
  struct is_blob : std::false_type
  {};

  /*! Forces field to be optional when empty. Concept requirements for `T` when
    `is_optional_on_empty<T>::value == true`:
      * must have an `empty()` method that toggles whether the associated
        `wire::field_<...>` is omitted by the `wire::writer`.
      * must have a `clear()` method where `empty() == true` upon completion,
        used by the `wire::reader` when the `wire::field_<...>` is omitted. */
  template<typename T>
  struct is_optional_on_empty
    : is_array<T> // all array types in old output engine were optional when empty
  {};

  // example usage : `wire::sum(std::size_t(wire::available(fields))...)`

  inline constexpr int sum() noexcept
  {
    return 0;
  }
  template<typename T, typename... U>
  inline constexpr T sum(const T head, const U... tail) noexcept
  {
    return head + sum(tail...);
  }

  //! If container has no `reserve(0)` function, this function is used
  template<typename... T>
  inline void reserve(const T&...) noexcept
  {}

  //! Container has `reserve(std::size_t)` function, use it
  template<typename T>
  inline auto reserve(T& container, const std::size_t count) -> decltype(container.reserve(count))
  { return container.reserve(count); }

  //! If `T` has no `empty()` function, this function is used
  template<typename... T>
  inline constexpr bool empty(const T&...) noexcept
  {
    static_assert(sum(is_optional_on_empty<T>::value...) == 0, "type needs empty method");
    return false;
  }

  //! `T` has `empty()` function, use it
  template<typename T>
  inline auto empty(const T& container) -> decltype(container.empty())
  { return container.empty(); }

  //! If `T` has no `clear()` function, this function is used
  template<typename... T>
  inline void clear(const T&...) noexcept
  {
    static_assert(sum(is_optional_on_empty<T>::value...) == 0, "type needs clear method");
  }

  //! `T` has `clear()` function, use it
  template<typename T>
  inline auto clear(T& container) -> decltype(container.clear())
  { return container.clear(); }
}
