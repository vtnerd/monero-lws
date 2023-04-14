// Copyright (c) 2020-2023, The Monero Project
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

#include <array>
#include <boost/utility/string_ref.hpp>
#include <cstdint>
#include <type_traits>

#include "byte_slice.h" // monero/contrib/epee/include
#include "span.h"       // monero/contrib/epee/include
#include "wire/field.h"
#include "wire/filters.h"
#include "wire/traits.h"

namespace wire
{
  //! Interface for converting C/C++ objects to "wire" (byte) formats.
  struct writer
  {
    writer() = default;

    virtual ~writer() noexcept;

    virtual void boolean(bool) = 0;

    virtual void integer(int) = 0;
    virtual void integer(std::intmax_t) = 0;

    virtual void unsigned_integer(unsigned) = 0;
    virtual void unsigned_integer(std::uintmax_t) = 0;

    virtual void real(double) = 0;

    virtual void string(boost::string_ref) = 0;
    virtual void binary(epee::span<const std::uint8_t> bytes) = 0;

    virtual void enumeration(std::size_t index, epee::span<char const* const> enums) = 0;

    virtual void start_array(std::size_t) = 0;
    virtual void end_array() = 0;

    virtual void start_object(std::size_t) = 0;
    virtual void key(std::uintmax_t) = 0;
    virtual void key(boost::string_ref) = 0;
    virtual void key(unsigned, boost::string_ref) = 0; //!< Implementation should output fastest key
    virtual void end_object() = 0;

  protected:
    writer(const writer&) = default;
    writer(writer&&) = default;
    writer& operator=(const writer&) = default;
    writer& operator=(writer&&) = default;
  };

  // leave in header, compiler can de-virtualize when final type is given

  inline void write_bytes(writer& dest, const bool source)
  {
    dest.boolean(source);
  }

  inline void write_bytes(writer& dest, const int source)
  {
    dest.integer(source);
  }
  inline void write_bytes(writer& dest, const long source)
  {
    dest.integer(std::intmax_t(source));
  }
  inline void write_bytes(writer& dest, const long long source)
  {
    dest.integer(std::intmax_t(source));
  }

  inline void write_bytes(writer& dest, const unsigned source)
  {
    dest.unsigned_integer(source);
  }
  inline void write_bytes(writer& dest, const unsigned long source)
  {
    dest.unsigned_integer(std::uintmax_t(source));
  }
  inline void write_bytes(writer& dest, const unsigned long long source)
  {
    dest.unsigned_integer(std::uintmax_t(source));
  }

  inline void write_bytes(writer& dest, const double source)
  {
    dest.real(source);
  }

  inline void write_bytes(writer& dest, const boost::string_ref source)
  {
    dest.string(source);
  }

  template<typename T>
  inline enable_if<is_blob<T>::value> write_bytes(writer& dest, const T& source)
  {
    dest.binary(epee::as_byte_span(source));
  }

  inline void write_bytes(writer& dest, const epee::span<const std::uint8_t> source)
  {
    dest.binary(source);
  }
}

namespace wire_write
{
  /*! Don't add a function called `write_bytes` to this namespace, it will
      prevent ADL lookup. ADL lookup delays the function searching until the
      template is used instead of when its defined. This allows the unqualified
      calls to `write_bytes` in this namespace to "find" user functions that are
      declared after these functions. */

  template<typename W, typename T>
  inline epee::byte_slice to_bytes(W&& dest, const T& value)
  {
    write_bytes(dest, value);
    return dest.take_bytes();
  }

  template<typename W, typename T>
  inline epee::byte_slice to_bytes(const T& value)
  {
    return wire_write::to_bytes(W{}, value);
  }

  template<typename W, typename T, typename F = wire::identity_>
  inline void array(W& dest, const T& source, const std::size_t count, F filter = F{})
  {
    using value_type = typename T::value_type;
    static_assert(!std::is_same<value_type, char>::value, "write array of chars as binary");
    static_assert(!std::is_same<value_type, std::uint8_t>::value, "write array of unsigned chars as binary");

    dest.start_array(count);
    for (const auto& elem : source)
      write_bytes(dest, filter(elem));
    dest.end_array();
  }

  template<typename W, typename T, unsigned I>
  inline bool field(W& dest, const wire::field_<T, true, I> elem)
  {
    // Arrays always optional, see `wire/field.h`
    if (wire::available(elem))
    {
      dest.key(I, elem.name);
      write_bytes(dest, elem.get_value());
    }
    return true;
  }

  template<typename W, typename T, unsigned I>
  inline bool field(W& dest, const wire::field_<T, false, I> elem)
  {
    if (wire::available(elem))
    {
      dest.key(I, elem.name);
      write_bytes(dest, *elem.get_value());
    }
    return true;
  }

  template<typename W, typename... T>
  inline void object(W& dest, T... fields)
  {
    dest.start_object(wire::sum(std::size_t(wire::available(fields))...));
    const bool dummy[] = {field(dest, std::move(fields))...};
    dest.end_object();
  }

  template<typename W, typename T, typename F, typename G>
  inline void dynamic_object(W& dest, const T& values, const std::size_t count, F key_filter, G value_filter)
  {
    dest.start_object(count);
    for (const auto& elem : values)
    {
      dest.key(key_filter(elem.first));
      write_bytes(dest, value_filter(elem.second));
    }
    dest.end_object();
  }
} // wire_write

namespace wire
{
  template<typename T, typename F = identity_>
  inline void array(writer& dest, const T& source, F filter = F{})
  {
    wire_write::array(dest, source, source.size(), std::move(filter));
  }
  template<typename T, typename F>
  inline void write_bytes(writer& dest, as_array_<T, F> source)
  {
    wire::array(dest, source.get_value(), std::move(source.filter));
  }
  template<typename T>
  inline enable_if<is_array<T>::value> write_bytes(writer& dest, const T& source)
  {
    wire::array(dest, source);
  }

  template<typename T, typename F = identity_, typename G = identity_>
  inline void dynamic_object(writer& dest, const T& source, F key_filter = F{}, G value_filter = G{})
  {
    wire_write::dynamic_object(dest, source, source.size(), std::move(key_filter), std::move(value_filter));
  }
  template<typename T, typename F, typename G>
  inline void write_bytes(writer& dest, as_object_<T, F, G> source)
  {
    wire::dynamic_object(dest, source.get_map(), std::move(source.key_filter), std::move(source.value_filter));
  }

  template<typename... T>
  inline void object(writer& dest, T... fields)
  {
    wire_write::object(dest, std::move(fields)...);
  }
}
