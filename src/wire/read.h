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

#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "common/expect.h" // monero/src
#include "span.h"          // monero/contrib/epee/include
#include "wire/error.h"
#include "wire/field.h"
#include "wire/traits.h"

namespace wire
{
  //! Interface for converting "wire" (byte) formats to C/C++ objects without a DOM.
  class reader
  {
    std::size_t depth_; //!< Tracks number of recursive objects and arrays

  protected:
    //! \throw wire::exception if max depth is reached
    void increment_depth();
    void decrement_depth() noexcept { --depth_; }

    reader(const reader&) = default;
    reader(reader&&) = default;
    reader& operator=(const reader&) = default;
    reader& operator=(reader&&) = default;

  public:
    struct key_map
    {
        const char* name;
        unsigned id; //<! For integer key formats;
    };

    //! \return Maximum read depth for both objects and arrays before erroring
    static constexpr std::size_t max_read_depth() noexcept { return 100; }

    //! \return Assume delimited arrays in generic interface (some optimizations disabled)
    static constexpr std::true_type delimited_arrays() noexcept { return {}; }

    reader() noexcept
      : depth_(0)
    {}

    virtual ~reader() noexcept
    {}

    //! \return Number of recursive objects and arrays
    std::size_t depth() const noexcept { return depth_; }

    //! \throw wire::exception if parsing is incomplete.
    virtual void check_complete() const = 0;

    //! \throw wire::exception if next value not a boolean.
    virtual bool boolean() = 0;

    //! \throw wire::exception if next value not an integer.
    virtual std::intmax_t integer() = 0;

    //! \throw wire::exception if next value not an unsigned integer.
    virtual std::uintmax_t unsigned_integer() = 0;

    //! \throw wire::exception if next value not number
    virtual double real() = 0;

    //! throw wire::exception if next value not string
    virtual std::string string() = 0;

    // ! \throw wire::exception if next value cannot be read as binary
    virtual std::vector<std::uint8_t> binary() = 0;

    //! \throw wire::exception if next value cannot be read as binary into `dest`.
    virtual void binary(epee::span<std::uint8_t> dest) = 0;

    //! \throw wire::exception if next value invalid enum. \return Index in `enums`.
    virtual std::size_t enumeration(epee::span<char const* const> enums) = 0;

    //! \throw wire::exception if next value not array
    virtual std::size_t start_array() = 0;

    //! \return True if there is another element to read.
    virtual bool is_array_end(std::size_t count) = 0;

    //! \throw wire::exception if array end delimiter not present.
    void end_array() noexcept { decrement_depth(); }


    //! \throw wire::exception if not object begin. \return State to be given to `key(...)` function.
    virtual std::size_t start_object() = 0;

    /*! Read a key of an object field and match against a known list of keys.
       Skips or throws exceptions on unknown fields depending on implementation
       settings.

      \param map of known keys (strings and integer) that are valid.
      \param[in,out] state returned by `start_object()` or `key(...)` whichever
        was last.
      \param[out] index of match found in `map`.

      \throw wire::exception if next value not a key.
      \throw wire::exception if next key not found in `map` and skipping
        fields disabled.

      \return True if this function found a field in `map` to process.
     */
    virtual bool key(epee::span<const key_map> map, std::size_t& state, std::size_t& index) = 0;

    void end_object() noexcept { decrement_depth(); }
  };

  template<typename R>
  inline void read_bytes(R& source, bool& dest)
  { dest = source.boolean(); }

  template<typename R>
  inline void read_bytes(R& source, double& dest)
  { dest = source.real(); }

  template<typename R>
  inline void read_bytes(R& source, std::string& dest)
  { dest = source.string(); }

  template<typename R>
  inline void read_bytes(R& source, std::vector<std::uint8_t>& dest)
  { dest = source.binary(); }

  template<typename R, typename T>
  inline std::enable_if_t<is_blob<T>::value> read_bytes(R& source, T& dest)
  { source.binary(epee::as_mut_byte_span(dest)); }

  namespace integer
  {
    [[noreturn]] void throw_exception(std::intmax_t value, std::intmax_t min, std::intmax_t max);
    [[noreturn]] void throw_exception(std::uintmax_t value, std::uintmax_t max);

    template<typename T, typename U>
    inline T cast_signed(const U source)
    {
      using limit = std::numeric_limits<T>;
      static_assert(
        std::is_signed<T>::value && std::is_integral<T>::value,
        "target must be signed integer type"
      );
      static_assert(
        std::is_signed<U>::value && std::is_integral<U>::value,
        "source must be signed integer type"
      );
      if (source < limit::min() || limit::max() < source)
       throw_exception(source, limit::min(), limit::max());
      return static_cast<T>(source);
    }

    template<typename T, typename U>
    inline T cast_unsigned(const U source)
    {
      using limit = std::numeric_limits<T>;
      static_assert(
        std::is_unsigned<T>::value && std::is_integral<T>::value,
        "target must be unsigned integer type"
      );
      static_assert(
        std::is_unsigned<U>::value && std::is_integral<U>::value,
        "source must be unsigned integer type"
      );
      if (limit::max() < source)
        throw_exception(source, limit::max());
      return static_cast<T>(source);
    }
  }

  //! read all current and future signed integer types
  template<typename R, typename T>
  inline std::enable_if_t<std::is_signed<T>::value && std::is_integral<T>::value>
  read_bytes(R& source, T& dest)
  {
    dest = integer::cast_signed<T>(source.integer());
  }

  //! read all current and future unsigned integer types
  template<typename R, typename T>
  inline std::enable_if_t<std::is_unsigned<T>::value && std::is_integral<T>::value>
  read_bytes(R& source, T& dest)
  {
    dest = integer::cast_unsigned<T>(source.unsigned_integer());
  }
} // wire

namespace wire_read
{
    /*! Don't add a function called `read_bytes` to this namespace, it will prevent
      ADL lookup. ADL lookup delays the function searching until the template
      is used instead of when its defined. This allows the unqualified calls to
      `read_bytes` in this namespace to "find" user functions that are declared
      after these functions (the technique behind `boost::serialization`). */

  [[noreturn]] void throw_exception(wire::error::schema code, const char* display, epee::span<char const* const> name_list);

  template<typename R, typename T>
  inline void bytes(R& source, T&& dest)
  {
    read_bytes(source, dest); // ADL (searches every associated namespace)
  }

  //! \return `T` converted from `source` or error.
  template<typename R, typename T, typename U>
  inline std::error_code from_bytes(T&& source, U& dest)
  {
    try
    {
      R in{std::forward<T>(source)};
      bytes(in, dest);
      in.check_complete();
    }
    catch (const wire::exception& e)
    {
      return e.code();
    }
    return {};
  }

  template<typename R, typename T>
  inline void array(R& source, T& dest)
  {
    using value_type = typename T::value_type;
    static_assert(!std::is_same<value_type, char>::value, "read array of chars as binary");
    static_assert(!std::is_same<value_type, std::uint8_t>::value, "read array of unsigned chars as binary");

    std::size_t count = source.start_array();

    dest.clear();
    wire::reserve(dest, count);

    bool more = count;
    while (more || !source.is_array_end(count))
    {
      dest.emplace_back();
      read_bytes(source, dest.back());
      --count;
      more &= bool(count);
    }

    return source.end_array();
  }

  template<typename T, unsigned I>
  inline void reset_field(wire::field_<T, true, I>& dest)
  {
    // array fields are always optional, see `wire/field.h`
    if (dest.optional_on_empty())
      wire::clear(dest.get_value());
  }

  template<typename T, unsigned I>
  inline void reset_field(wire::field_<T, false, I>& dest)
  {
    dest.get_value().reset();
  }

  template<typename R, typename T, unsigned I>
  inline void unpack_field(std::size_t, R& source, wire::field_<T, true, I>& dest)
  {
    bytes(source, dest.get_value());
  }

  template<typename R, typename T, unsigned I>
  inline void unpack_field(std::size_t, R& source, wire::field_<T, false, I>& dest)
  {
    if (!bool(dest.get_value()))
      dest.get_value().emplace();
    bytes(source, *dest.get_value());
  }

  //! Tracks read status of every object field instance.
  template<typename T>
  class tracker
  {
    T field_;
    std::size_t our_index_;
    bool read_;

  public:
    static constexpr bool is_required() noexcept { return T::is_required(); }
    static constexpr std::size_t count() noexcept { return T::count(); }

    explicit tracker(T field)
      : field_(std::move(field)), our_index_(0), read_(false)
    {}

    //! \return Field name if required and not read, otherwise `nullptr`.
    const char* name_if_missing() const noexcept
    {
      return (is_required() && !read_) ? field_.name : nullptr;
    }


    //! Set all entries in `map` related to this field (expand variant types!).
    template<std::size_t N>
    std::size_t set_mapping(std::size_t index, wire::reader::key_map (&map)[N])
    {
      our_index_ = index;
      map[index].id = field_.id();
      map[index].name = field_.name;
      return index + count();
    }

    //! Try to read next value if `index` matches `this`. \return 0 if no match, 1 if optional field read, and 2 if required field read
    template<typename R>
    std::size_t try_read(R& source, const std::size_t index)
    {
      if (index < our_index_ || our_index_ + count() <= index)
        return 0;
      if (read_)
        throw_exception(wire::error::schema::invalid_key, "duplicate", {std::addressof(field_.name), 1});

      unpack_field(index - our_index_, source, field_);
      read_ = true;
      return 1 + is_required();
    }

    //! Reset optional fields that were skipped
    bool reset_omitted()
    {
      if (!is_required() && !read_)
        reset_field(field_);
      return true;
    }
  };

  // `expand_tracker_map` writes all `tracker` types to a table

  template<std::size_t N>
  inline constexpr std::size_t expand_tracker_map(std::size_t index, const wire::reader::key_map (&)[N])
  {
    return index;
  }

  template<std::size_t N, typename T, typename... U>
  inline void expand_tracker_map(std::size_t index, wire::reader::key_map (&map)[N], tracker<T>& head, tracker<U>&... tail)
  {
    expand_tracker_map(head.set_mapping(index, map), map, tail...);
  }

  template<typename R, typename... T>
  inline void object(R& source, tracker<T>... fields)
  {
    static constexpr const std::size_t total_subfields = wire::sum(fields.count()...);
    static_assert(total_subfields < 100, "algorithm uses too much stack space and linear searching");

    std::size_t state = source.start_object();
    std::size_t required = wire::sum(std::size_t(fields.is_required())...);

    wire::reader::key_map map[total_subfields] = {};
    expand_tracker_map(0, map, fields...);

    std::size_t next = 0;
    while (source.key(map, state, next))
    {
      switch (wire::sum(fields.try_read(source, next)...))
      {
      default:
      case 0:
        throw_exception(wire::error::schema::invalid_key, "bad map setup", nullptr);
        break;
      case 2:
        --required; /* fallthrough */
      case 1:
        break;
      }
    }

    if (required)
    {
      const char* missing[] = {fields.name_if_missing()...};
      throw_exception(wire::error::schema::missing_key, "", missing);
    }

    wire::sum(fields.reset_omitted()...);
    source.end_object();
  }
} // wire_read

namespace wire
{
  template<typename R, typename T>
  inline std::enable_if_t<is_array<T>::value> read_bytes(R& source, T& dest)
  {
    wire_read::array(source, dest);
  }

  template<typename R, typename... T>
  inline std::enable_if_t<std::is_base_of<reader, R>::value> object(R& source, T... fields)
  {
    wire_read::object(source, wire_read::tracker<T>{std::move(fields)}...);
  }
}
