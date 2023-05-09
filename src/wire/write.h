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
#include <boost/range/size.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <cstdint>
#include <type_traits>
#include <system_error>

#include "byte_slice.h" // monero/contrib/epee/include
#include "byte_stream.h"// monero/contrib/epee/include
#include "span.h"       // monero/contrib/epee/include
#include "wire/error.h"
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

    //! By default, insist on retrieving array size before writing array
    static constexpr std::true_type need_array_size() noexcept { return{}; }

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

  template<typename W>
  inline void write_bytes(W& dest, const bool source)
  { dest.boolean(source); }

  template<typename W>
  inline void write_arithmetic(W& dest, const int source)
  { dest.integer(source); }

  template<typename W>
  inline void write_arithmetic(W& dest, const long source)
  { dest.integer(std::intmax_t(source)); }

  template<typename W>
  inline void write_arithmetic(W& dest, const long long source)
  { dest.integer(std::intmax_t(source)); }

  template<typename W>
  inline void write_arithmetic(W& dest, const unsigned source)
  { dest.unsigned_integer(source); }

  template<typename W>
  inline void write_arithmetic(W& dest, const unsigned long source)
  { dest.unsigned_integer(std::uintmax_t(source)); }

  template<typename W>
  inline void write_arithmetic(W& dest, const unsigned long long source)
  { dest.unsigned_integer(std::uintmax_t(source)); }

  template<typename W, typename T>
  inline std::enable_if_t<std::is_arithmetic<T>::value> write_bytes(W& dest, const T source)
  { write_arithmetic(dest, source); }

  template<typename W>
  inline void write_bytes(W& dest, const double source)
  { dest.real(source); }

  template<typename W>
  inline void write_bytes(W& dest, const boost::string_ref source)
  { dest.string(source); }

  template<typename W, typename T>
  inline std::enable_if_t<is_blob<T>::value> write_bytes(W& dest, const T& source)
  { dest.binary(epee::as_byte_span(source)); }

  template<typename W>
  inline void write_bytes(W& dest, const epee::span<const std::uint8_t> source)
  { dest.binary(source); }

  template<typename W>
  inline void write_bytes(W& dest, const epee::byte_slice& source)
  { write_bytes(dest, epee::to_span(source)); }

  //! Use `write_bytes(...)` method if available for `T`.
  template<typename W, typename T>
  inline auto write_bytes(W& dest, const T& source) -> decltype(source.write_bytes(dest))
  { return source.write_bytes(dest); }
}

namespace wire_write
{
  /*! Don't add a function called `write_bytes` to this namespace, it will
      prevent ADL lookup. ADL lookup delays the function searching until the
      template is used instead of when its defined. This allows the unqualified
      calls to `write_bytes` in this namespace to "find" user functions that are
      declared after these functions. */

  template<typename W, typename T>
  inline void bytes(W& dest, const T& source)
  {
    write_bytes(dest, source); // ADL (searches every associated namespace)
  }

  template<typename W, typename T, typename U>
  inline std::error_code to_bytes(T& dest, const U& source)
  {
    try
    {
      W out{std::move(dest)};
      bytes(out, source);
      dest = out.take_sink();
    }
    catch (const wire::exception& e)
    {
      dest.clear();
      return e.code();
    }
    catch (...)
    {
      dest.clear();
      throw;
    }
    return {};
  }

  template<typename W, typename T>
  inline std::error_code to_bytes(epee::byte_slice& dest, const T& source)
  {
    epee::byte_stream sink{};
    const std::error_code error = wire_write::to_bytes<W>(sink, source);
    if (error)
    {
      dest = nullptr;
      return error;
    }
    dest = epee::byte_slice{std::move(sink)};
    return {};
  }

  template<typename T>
  inline std::size_t array_size_(std::true_type, const T& source)
  { return boost::size(source); }

  template<typename T>
  inline constexpr std::size_t array_size_(std::false_type, const T&) noexcept
  { return 0; }

  template<typename W, typename T>
  inline constexpr std::size_t array_size(const W& dest, const T& source) noexcept
  { return array_size_(dest.need_array_size(), source); }

  template<typename W, typename T>
  inline void array(W& dest, const T& source)
  {
    using value_type = typename T::value_type;
    static_assert(!std::is_same<value_type, char>::value, "write array of chars as binary");
    static_assert(!std::is_same<value_type, std::uint8_t>::value, "write array of unsigned chars as binary");

    dest.start_array(array_size(dest, source));
    for (const auto& elem : source)
      bytes(dest, elem);
    dest.end_array();
  }

  template<typename W, typename T, unsigned I>
  inline bool field(W& dest, const wire::field_<T, true, I> elem)
  {
    // Arrays always optional, see `wire::field.h`
    if (wire::available(elem))
    {
      dest.key(I, elem.name);
      bytes(dest, elem.get_value());
    }
    return true;
  }

  template<typename W, typename T, unsigned I>
  inline bool field(W& dest, const wire::field_<T, false, I> elem)
  {
    if (wire::available(elem))
    {
      dest.key(I, elem.name);
      bytes(dest, *elem.get_value());
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
  inline void dynamic_object(W& dest, const T& values, F key_filter, G value_filter)
  {
    dest.start_object(array_size(dest, values));
    for (const auto& elem : values)
    {
      dest.key(key_filter(elem.first));
      bytes(dest, value_filter(elem.second));
    }
    dest.end_object();
  }
} // wire_write

namespace wire
{
  template<typename W, typename T, typename F>
  inline void write_bytes(W& dest, const as_array_<T, F> source)
  {
    wire_write::array(dest, boost::adaptors::transform(source.get_value(), source.filter));
  }
  template<typename W, typename T>
  inline std::enable_if_t<is_array<T>::value> write_bytes(W& dest, const T& source)
  {
    wire_write::array(dest, source);
  }

  template<typename W, typename T, typename F = identity_, typename G = identity_>
  inline std::enable_if_t<std::is_base_of<writer, W>::value>
  dynamic_object(W& dest, const T& source, F key_filter = F{}, G value_filter = G{})
  {
    wire_write::dynamic_object(dest, source, std::move(key_filter), std::move(value_filter));
  }
  template<typename W, typename T, typename F, typename G>
  inline void write_bytes(W& dest, as_object_<T, F, G> source)
  {
    wire::dynamic_object(dest, source.get_map(), std::move(source.key_filter), std::move(source.value_filter));
  }

  template<typename W, typename... T>
  inline std::enable_if_t<std::is_base_of<writer, W>::value> object(W& dest, T... fields)
  {
    wire_write::object(dest, std::move(fields)...);
  }
}
