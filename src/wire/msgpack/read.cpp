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

#include "read.h"

#include <boost/endian/buffers.hpp>
#include <boost/fusion/include/any.hpp>
#include <boost/fusion/include/std_tuple.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "wire/error.h"
#include "wire/msgpack/error.h"

// Expands to every possible fixed string tag value
#define MLWS_FIXED_STRING_TAGS()                                \
  case wire::msgpack::tag(0xa0): case wire::msgpack::tag(0xa1): \
  case wire::msgpack::tag(0xa2): case wire::msgpack::tag(0xa3): \
  case wire::msgpack::tag(0xa4): case wire::msgpack::tag(0xa5): \
  case wire::msgpack::tag(0xa6): case wire::msgpack::tag(0xa7): \
  case wire::msgpack::tag(0xa8): case wire::msgpack::tag(0xa9): \
  case wire::msgpack::tag(0xaa): case wire::msgpack::tag(0xab): \
  case wire::msgpack::tag(0xac): case wire::msgpack::tag(0xad): \
  case wire::msgpack::tag(0xae): case wire::msgpack::tag(0xaf): \
  case wire::msgpack::tag(0xb0): case wire::msgpack::tag(0xb1): \
  case wire::msgpack::tag(0xb2): case wire::msgpack::tag(0xb3): \
  case wire::msgpack::tag(0xb4): case wire::msgpack::tag(0xb5): \
  case wire::msgpack::tag(0xb6): case wire::msgpack::tag(0xb7): \
  case wire::msgpack::tag(0xb8): case wire::msgpack::tag(0xb9): \
  case wire::msgpack::tag(0xba): case wire::msgpack::tag(0xbb): \
  case wire::msgpack::tag(0xbc): case wire::msgpack::tag(0xbd): \
  case wire::msgpack::tag(0xbe): case wire::msgpack::tag(0xbf):

namespace
{
  template<typename T>
  using limits = std::numeric_limits<T>;

  //! \return True iif `value` matches a tag in `T` tuple.
  template<typename T>
  bool matches(const wire::msgpack::tag tag)
  {
    const auto matched_type = [tag] (const auto type)
    {
      return type.Tag() == tag;
    };
    // NOTE: This is slower than a switch but more flexible/reusable
    return boost::fusion::any(T{}, matched_type);
  }

  //! \return Integer `T` encoded as big endian in `source`.
  template<typename T>
  T read_endian(epee::span<const std::uint8_t>& source)
  {
    static_assert(std::is_integral<T>::value, "must be integral type");
    static constexpr const std::size_t bits = 8 * sizeof(T);
    using buffer_type =
      boost::endian::endian_buffer<boost::endian::order::big, T, bits>;

    buffer_type buffer;
    static_assert(sizeof(buffer) == sizeof(T), "unexpected buffer size");
    if (source.size() < sizeof(buffer))
      WIRE_DLOG_THROW_(wire::error::msgpack::not_enough_bytes);
    std::memcpy(std::addressof(buffer), source.data(), sizeof(buffer));
    source.remove_prefix(sizeof(buffer));
    return buffer.value();
  }

  //! \return Integer `T` encoded as big endian in `source`.
  template<typename T, wire::msgpack::tag U>
  T read_endian(epee::span<const std::uint8_t>& source, const wire::msgpack::type<T, U>)
  { return read_endian<T>(source); }

  //! \return Integer `T` whose encoding is specified by tag `next`
  template<typename T>
  T read_integer(epee::span<const std::uint8_t>& source, const wire::msgpack::tag next)
  {
    try
    {
      // msgpack::integer_types
      switch (next)
      {
        default:
          break;
        case wire::msgpack::tag::int8:
          return boost::numeric_cast<T>(read_endian<std::int8_t>(source));
        case wire::msgpack::tag::uint8:
          return boost::numeric_cast<T>(read_endian<std::uint8_t>(source));
        case wire::msgpack::tag::int16:
          return boost::numeric_cast<T>(read_endian<std::int16_t>(source));
        case wire::msgpack::tag::uint16:
          return boost::numeric_cast<T>(read_endian<std::uint16_t>(source));
        case wire::msgpack::tag::int32:
          return boost::numeric_cast<T>(read_endian<std::int32_t>(source));
        case wire::msgpack::tag::uint32:
          return boost::numeric_cast<T>(read_endian<std::uint32_t>(source));
        case wire::msgpack::tag::int64:
          return boost::numeric_cast<T>(read_endian<std::int64_t>(source));
        case wire::msgpack::tag::uint64:
          return boost::numeric_cast<T>(read_endian<std::uint64_t>(source));
      }
    }
    catch (const boost::numeric::positive_overflow&)
    { WIRE_DLOG_THROW_(wire::error::schema::smaller_integer); }
    catch (const boost::numeric::negative_overflow&)
    { WIRE_DLOG_THROW_(wire::error::schema::larger_integer); }
    
    WIRE_DLOG_THROW_(wire::error::schema::integer);
  }

  epee::span<const std::uint8_t> read_raw(epee::span<const std::uint8_t>& source, const std::size_t bytes)
  {
    if (source.size() < bytes)
      WIRE_DLOG_THROW_(wire::error::msgpack::not_enough_bytes);
    const std::size_t actual = source.remove_prefix(bytes);
    return {source.data() - actual, actual};
  }

  template<typename T>
  epee::span<const std::uint8_t> read_raw(epee::span<const std::uint8_t>& source)
  {
    return read_raw(source, wire::integer::cast_unsigned<std::size_t>(read_endian<T>(source)));
  }

  epee::span<const std::uint8_t> read_string(epee::span<const std::uint8_t>& source, const wire::msgpack::tag next)
  {
    switch (next)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
      MLWS_FIXED_STRING_TAGS()
        return read_raw(source, wire::msgpack::ftag_string::extract(next));
#pragma GCC diagnostic pop
      case wire::msgpack::tag::string8:
        return read_raw<std::uint8_t>(source);
      case wire::msgpack::tag::string16:
        return read_raw<std::uint16_t>(source);
      case wire::msgpack::tag::string32:
        return read_raw<std::uint32_t>(source);
      default:
        break;
    }
    WIRE_DLOG_THROW_(wire::error::schema::string);
  }

  //! \return Binary blob encoded message
  epee::span<const std::uint8_t> read_binary(epee::span<const std::uint8_t>& source, const wire::msgpack::tag next)
  {
    switch (next)
    {
      case wire::msgpack::tag::binary8:
        return read_raw<std::uint8_t>(source);
      case wire::msgpack::tag::binary16:
        return read_raw<std::uint16_t>(source);
      case wire::msgpack::tag::binary32:
        return read_raw<std::uint32_t>(source);
      default:
        break;
    }
    WIRE_DLOG_THROW_(wire::error::schema::string);
  }
}

namespace wire
{
  void msgpack_reader::throw_wire_exception()
  {
    WIRE_DLOG_THROW_(error::msgpack::underflow_tree);
  }

  void msgpack_reader::skip_value()
  {
    assert(tags_remaining_);
    if (limits<std::size_t>::max() == tags_remaining_)
      WIRE_DLOG_THROW_(error::msgpack::max_tree_size);

    const std::size_t initial = tags_remaining_;
    do
    {
      const std::size_t size = remaining_.size();
      const msgpack::tag next = peek_tag();
      switch (next)
      {
        default:
          break;
        case msgpack::tag::nil:
        case msgpack::tag::unused:
        case msgpack::tag::False:
        case msgpack::tag::True:
          remaining_.remove_prefix(1);
          break;
        case msgpack::tag::binary8:
        case msgpack::tag::binary16:
        case msgpack::tag::binary32:
          remaining_.remove_prefix(1);
          read_binary(remaining_, next);
          break;
        case msgpack::tag::extension8:
          remaining_.remove_prefix(1);
          read_raw<std::uint8_t>(remaining_);
          remaining_.remove_prefix(1);
          break;
        case msgpack::tag::extension16:
          remaining_.remove_prefix(1);
          read_raw<std::uint16_t>(remaining_);
          remaining_.remove_prefix(1);
          break;
        case msgpack::tag::extension32:
          remaining_.remove_prefix(1);
          read_raw<std::uint32_t>(remaining_);
          remaining_.remove_prefix(1);
          break;
        case msgpack::tag::int8:
        case msgpack::tag::uint8:
          remaining_.remove_prefix(2);
          break;
        case msgpack::tag::int16:
        case msgpack::tag::uint16:
        case msgpack::tag::fixed_extension1:
          remaining_.remove_prefix(3);
          break;
        case msgpack::tag::int32:
        case msgpack::tag::uint32:
        case msgpack::tag::float32:
          remaining_.remove_prefix(5);
          break;
        case msgpack::tag::int64:
        case msgpack::tag::uint64:
        case msgpack::tag::float64:
          remaining_.remove_prefix(9);
          break;
        case msgpack::tag::fixed_extension2:
          remaining_.remove_prefix(4);
          break;
        case msgpack::tag::fixed_extension4:
          remaining_.remove_prefix(6);
          break;
        case msgpack::tag::fixed_extension8:
          remaining_.remove_prefix(10);
          break;
        case msgpack::tag::fixed_extension16:
          remaining_.remove_prefix(18);
          break;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
        MLWS_FIXED_STRING_TAGS()
        case msgpack::tag::string8:
        case msgpack::tag::string16:
        case msgpack::tag::string32:
          remaining_.remove_prefix(1);
          read_string(remaining_, next);
          break;
        case msgpack::tag(0x90): case msgpack::tag(0x91): case msgpack::tag(0x92):
        case msgpack::tag(0x93): case msgpack::tag(0x94): case msgpack::tag(0x95):
        case msgpack::tag(0x96): case msgpack::tag(0x97): case msgpack::tag(0x98):
        case msgpack::tag(0x99): case msgpack::tag(0x9a): case msgpack::tag(0x9b):
        case msgpack::tag(0x9c): case msgpack::tag(0x9d): case msgpack::tag(0x9e):
        case msgpack::tag(0x9f):
        case msgpack::tag::array16:
        case msgpack::tag::array32:
          start_array(0);
          break;
        case msgpack::tag(0x80): case msgpack::tag(0x81): case msgpack::tag(0x82):
        case msgpack::tag(0x83): case msgpack::tag(0x84): case msgpack::tag(0x85):
        case msgpack::tag(0x86): case msgpack::tag(0x87): case msgpack::tag(0x88):
        case msgpack::tag(0x89): case msgpack::tag(0x8a): case msgpack::tag(0x8b):
        case msgpack::tag(0x8c): case msgpack::tag(0x8d): case msgpack::tag(0x8e):
        case msgpack::tag(0x8f):
        case msgpack::tag::object16:
        case msgpack::tag::object32:
          start_object();
          break;
#pragma GCC diagnostic pop
      };

      if (size == remaining_.size())
      {
        if (!msgpack::ftag_unsigned::matches(next) && !msgpack::ftag_signed::matches(next))
          WIRE_DLOG_THROW_(error::msgpack::invalid);
        remaining_.remove_prefix(1);
      }
      update_tags_remaining();
    } while (initial <= tags_remaining_);
  }

  msgpack::tag msgpack_reader::peek_tag()
  {
    if (remaining_.empty())
      WIRE_DLOG_THROW_(error::msgpack::not_enough_bytes);
    return msgpack::tag(*remaining_.data());
  }

  msgpack::tag msgpack_reader::get_tag()
  {
    const msgpack::tag next = peek_tag();
    remaining_.remove_prefix(1);
    return next;
  }

  std::intmax_t msgpack_reader::do_integer(const msgpack::tag next)
  {
    if (msgpack::ftag_signed::matches(next))
      return *reinterpret_cast<const std::int8_t*>(std::addressof(next)); // special case
    return read_integer<std::intmax_t>(remaining_, next);
  }

  std::uintmax_t msgpack_reader::do_unsigned_integer(const msgpack::tag next)
  {
    return read_integer<std::uintmax_t>(remaining_, next);
  }

  template<typename T, typename U>
  std::size_t msgpack_reader::read_count(const error::schema expected)
  {
    const msgpack::tag next = get_tag();
    if (T::matches(next))
      return T::extract(next);

    std::size_t out = 0;
    const auto matched_type = [this, &out, next](const auto type)
    {
      if (type.Tag() == next)
      {
        out = integer::cast_unsigned<std::size_t>(read_endian(remaining_, type));
        return true;
      }
      return false;
    };

    if (!boost::fusion::any(U{}, matched_type))
      WIRE_DLOG_THROW_(expected);

    return out;
  }

  void msgpack_reader::check_complete() const
  {
    if (tags_remaining_)
      WIRE_DLOG_THROW_(error::msgpack::incomplete);
  }

  bool msgpack_reader::boolean()
  {
    update_tags_remaining();
    switch (get_tag())
    {
      case msgpack::tag::True:
        return true;
      case msgpack::tag::False:
        return false;
      default:
        break;
    }
    WIRE_DLOG_THROW_(error::schema::boolean);
  }

  double msgpack_reader::real()
  {
    update_tags_remaining();

    const auto read_float = [this](auto value)
    {
      if (remaining_.size() < sizeof(value))
        WIRE_DLOG_THROW_(error::msgpack::not_enough_bytes);
      std::memcpy(std::addressof(value), remaining_.data(), sizeof(value));
      remaining_.remove_prefix(sizeof(value));
      return value;
    };

    switch (get_tag())
    {
      case msgpack::tag::float32:
        return read_float(float(0));
      case msgpack::tag::float64:
        return read_float(double(0));
      default:
        break;
    }
    WIRE_DLOG_THROW_(error::schema::number);
  }

  std::string msgpack_reader::string()
  {
    update_tags_remaining();
    const epee::span<const std::uint8_t> bytes = read_string(remaining_, get_tag());
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

  std::vector<std::uint8_t> msgpack_reader::binary()
  {
    update_tags_remaining();
    const epee::span<const std::uint8_t> bytes = read_binary(remaining_, get_tag());
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
  }

  void msgpack_reader::binary(epee::span<std::uint8_t> dest)
  {
    update_tags_remaining();
    const epee::span<const std::uint8_t> bytes = read_binary(remaining_, get_tag());
    if (dest.size() != bytes.size())
      WIRE_DLOG_THROW(error::schema::fixed_binary, "of size " << dest.size() << " but got " << bytes.size());
    std::memcpy(dest.data(), bytes.data(), dest.size());
  }

  std::size_t msgpack_reader::start_array(const std::size_t min_element_size)
  {
    const std::size_t upcoming =
      read_count<msgpack::ftag_array, msgpack::array_types>(error::schema::array);
    if (limits<std::size_t>::max() - tags_remaining_ < upcoming)
      WIRE_DLOG_THROW_(error::msgpack::max_tree_size);
    if (min_element_size && (remaining_.size() / min_element_size) < upcoming)
      WIRE_DLOG_THROW(error::schema::array, upcoming << " array elements of at least " << min_element_size << " bytes each exceeds " << remaining_.size() << " remaining bytes");

    tags_remaining_ += upcoming;
    increment_depth();
    return upcoming;
  }

  bool msgpack_reader::is_array_end(const std::size_t count)
  {
    if (count)
      return false;
    update_tags_remaining();
    return true;
  }

  std::size_t msgpack_reader::start_object()
  {
    const std::size_t upcoming =
      read_count<msgpack::ftag_object, msgpack::object_types>(error::schema::object);
    if (limits<std::size_t>::max() / 2 < upcoming)
      WIRE_DLOG_THROW_(error::msgpack::max_tree_size);
    if (limits<std::size_t>::max() - tags_remaining_ < upcoming * 2)
      WIRE_DLOG_THROW_(error::msgpack::max_tree_size);
    tags_remaining_ += upcoming * 2;
    increment_depth();
    return upcoming;
  }

  bool msgpack_reader::key(const epee::span<const key_map> map, std::size_t& state, std::size_t& index)
  {
    index = map.size();
    for ( ;state; --state)
    {
      update_tags_remaining(); // for key
      const msgpack::tag next = get_tag();
      const bool single = msgpack::ftag_unsigned::matches(next);
      if (single || matches<msgpack::unsigned_types>(next))
      {
        unsigned key = std::uint8_t(next);
        if (!single)
          key = read_integer<unsigned>(remaining_, next);
        for (const key_map& elem : map)
        {
          if (elem.id == key)
          {
            index = std::addressof(elem) - map.begin();
            break;
          }
        }
      }
      else if (msgpack::ftag_string::matches(next) || matches<msgpack::string_types>(next))
      {
        const epee::span<const std::uint8_t> key = read_string(remaining_, next);
        for (const key_map& elem : map)
        {
          const boost::string_ref elem_{elem.name};
          if (key.size() == elem_.size() && std::memcmp(key.data(), elem_.data(), key.size()) == 0)
          {
            index = std::addressof(elem) - map.begin();
            break;
          }
        }
      }
      else
        WIRE_DLOG_THROW(error::schema::invalid_key, "Invalid key type");

      if (index < map.size())
      {
        --state;
        return true;
      }
      skip_value();
    } // until state == 0
    update_tags_remaining(); // for end of object
    return false;
  }
}
