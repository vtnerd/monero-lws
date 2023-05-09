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

#include "write.h"

#include <boost/endian/buffers.hpp>
#include <boost/fusion/include/any.hpp>
#include <boost/fusion/include/std_tuple.hpp>
#include <cassert>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <type_traits>

#include "wire/error.h"
#include "wire/msgpack/error.h"

namespace
{
  template<typename T>
  using limits = std::numeric_limits<T>;

  constexpr const unsigned flush_threshold = 100;
  constexpr const unsigned max_buffer = 4096;

  void write_tag(epee::byte_stream& bytes, const wire::msgpack::tag value)
  {
    bytes.put(std::uint8_t(value));
  }

  template<typename T, typename U, wire::msgpack::tag tag>
  void write_endian(epee::byte_stream& bytes, const T value, const wire::msgpack::type<U, tag> type)
  {
    static_assert(std::is_integral<T>::value, "input not integral");
    static_assert(std::is_integral<U>::value, "output not integral");

    using in_limits = std::numeric_limits<T>;
    using out_limits = std::numeric_limits<U>;
    static_assert(in_limits::is_signed == out_limits::is_signed, "signs must match");

    assert(type.min() <= value);
    assert(value <= type.max());

    static constexpr const std::size_t bits = 8 * sizeof(U);
    using buffer_type =
      boost::endian::endian_buffer<boost::endian::order::big, U, bits>;

    buffer_type buffer(value);
    write_tag(bytes, type.Tag());
    bytes.write(buffer.data(), sizeof(buffer));
  }

  template<typename T>
  void write_count(epee::byte_stream& bytes, const std::uintmax_t items)
  {
    const auto match_size = [&bytes, items] (const auto type) -> bool
    {
      if (type.max() < items)
        return false;
      write_endian(bytes, items, type);
      return true;
    };
    if (!boost::fusion::any(T{}, match_size))
      WIRE_DLOG_THROW_(wire::error::msgpack::integer_encoding);
  }

  template<typename T, typename U>
  void write_count(epee::byte_stream& bytes, const std::uintmax_t items)
  {
    if (items <= T::max())
      bytes.put(T::tag() | std::uint8_t(items));
    else
      write_count<U>(bytes, items);
  }
}

namespace wire
{
  void msgpack_writer::do_flush(epee::span<const std::uint8_t>)
  {}

  void msgpack_writer::check_flush()
  {
    if (needs_flush_ && (max_buffer < bytes_.size() || bytes_.available() < flush_threshold))
      flush();
  }

  void msgpack_writer::do_integer(const std::intmax_t value)
  {
    assert(value < 0); // constraint checked in header
    if (0xe0 < value) // 0xe0 needs to be type `int` to work
    {
      bytes_.put(std::uint8_t(value));
      return;
    }

    const auto match_size = [this, value] (const auto type) -> bool
    {
      if (value < type.min())
        return false;
      write_endian(bytes_, value, type);
      return true;
    };
    if (!boost::fusion::any(wire::msgpack::signed_types{}, match_size))
      WIRE_DLOG_THROW_(wire::error::msgpack::integer_encoding);
  }

  void msgpack_writer::do_unsigned_integer(const std::uintmax_t value)
  {
    const auto match_size = [this, value] (const auto type) -> bool
    {
      if (type.max() < value)
        return false;
      write_endian(bytes_, value, type);
      return true;
    };
    if (!boost::fusion::any(wire::msgpack::unsigned_types{}, match_size))
      WIRE_DLOG_THROW_(wire::error::msgpack::integer_encoding);
  }

  void msgpack_writer::check_complete()
  {
    if (expected_)
      throw std::logic_error{"msgpack_writer::take_msgpack() failed with incomplete tree"};
  }
  epee::byte_stream msgpack_writer::take_msgpack()
  {
    check_complete();
    epee::byte_stream out{std::move(bytes_)};
    bytes_.clear();
    return out;
  }

  msgpack_writer::~msgpack_writer() noexcept
  {}

  void msgpack_writer::real(const double source)
  {
    write_tag(bytes_, msgpack::tag::float64);
    bytes_.write(reinterpret_cast<const char*>(std::addressof(source)), sizeof(source));
    --expected_;
  }

  void msgpack_writer::string(const boost::string_ref source)
  {
    write_count<msgpack::ftag_string, msgpack::string_types>(bytes_, source.size());
    bytes_.write(source.data(), source.size());
    --expected_;
  }
  void msgpack_writer::binary(epee::span<const std::uint8_t> source)
  {
    write_count<msgpack::binary_types>(bytes_, source.size());
    bytes_.write(source);
    --expected_;
  }

  void msgpack_writer::enumeration(const std::size_t index, const epee::span<char const* const> enums)
  {
    if (enums.size() < index)
      throw std::logic_error{"Invalid enum/string value"};
    unsigned_integer(index);
  }

  void msgpack_writer::start_array(const std::size_t items)
  {
    write_count<msgpack::ftag_array, msgpack::array_types>(bytes_, items);
    expected_ += items;
  }

  void msgpack_writer::start_object(const std::size_t items)
  {
    write_count<msgpack::ftag_object, msgpack::object_types>(bytes_, items);
    expected_ += items * 2;
  }
  void msgpack_writer::key(const boost::string_ref str)
  {
    string(str);
  }
  void msgpack_writer::key(const std::uintmax_t id)
  {
    unsigned_integer(id);
  }
  void msgpack_writer::key(const unsigned id, boost::string_ref str)
  {
    if (integer_keys_)
      key(id);
    else
      key(str);
  }

  void msgpack_stream_writer::do_flush(epee::span<const std::uint8_t> bytes)
  {
    dest.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
}
