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

#include "write.h"

#include <ostream>
#include <stdexcept>

#include "hex.h" // monero/contrib/epee/include

namespace
{
  constexpr const unsigned flush_threshold = 100;
  constexpr const unsigned max_buffer = 4096;
}

namespace wire
{
  void json_writer::do_flush(epee::span<const std::uint8_t>)
  {}

  void json_writer::check_flush()
  {
    if (needs_flush_ && (max_buffer < bytes_.size() || bytes_.available() < flush_threshold))
      flush();
  }

  void json_writer::check_complete()
  {
    if (!formatter_.IsComplete())
      throw std::logic_error{"json_writer::take_json() failed with incomplete JSON tree"};
  }
  epee::byte_stream json_writer::take_json()
  {
    check_complete();
    epee::byte_stream out{std::move(bytes_)};
    bytes_.clear();
    formatter_.Reset(bytes_);
    return out;
  }

  json_writer::~json_writer() noexcept
  {}

  std::array<char, uint_to_string_size> json_writer::to_string(const std::uintmax_t value) noexcept
  {
    static_assert(std::numeric_limits<std::uintmax_t>::max() <= std::numeric_limits<std::uint64_t>::max(), "bad uint conversion");
    std::array<char, uint_to_string_size> buf{{}};
    rapidjson::internal::u64toa(std::uint64_t(value), buf.data());
    return buf;
  }

  void json_writer::boolean(const bool source)
  {
    formatter_.Bool(source);
    check_flush();
  }

  void json_writer::integer(const int source)
  {
    formatter_.Int(source);
    check_flush();
  }
  void json_writer::integer(const std::intmax_t source)
  {
    static_assert(std::numeric_limits<std::int64_t>::min() <= std::numeric_limits<std::intmax_t>::min(), "too small");
    static_assert(std::numeric_limits<std::intmax_t>::max() <= std::numeric_limits<std::int64_t>::max(), "too large");
    formatter_.Int64(source);
    check_flush();
  }
  void json_writer::unsigned_integer(const unsigned source)
  {
    formatter_.Uint(source);
    check_flush();
  }
    void json_writer::unsigned_integer(const std::uintmax_t source)
  {
    static_assert(std::numeric_limits<std::uintmax_t>::max() <= std::numeric_limits<std::uint64_t>::max(), "too large");
    formatter_.Uint64(source);
    check_flush();
  }
  void json_writer::real(const double source)
  {
    formatter_.Double(source);
    check_flush();
  }

  void json_writer::string(const boost::string_ref source)
  {
    formatter_.String(source.data(), source.size());
    check_flush();
  }
  void json_writer::binary(epee::span<const std::uint8_t> source)
  {/* TODO update monero project
    std::array<char, 256> buffer;
    if (source.size() <= buffer.size() / 2)
    {
      if (!epee::to_hex::buffer({buffer.data(), source.size() * 2}, source))
        throw std::logic_error{"Invalid buffer size for binary->hex conversion"};
      string({buffer.data(), source.size() * 2});
    }
    else
    {*/
      const auto hex = epee::to_hex::string(source);
      string(hex);
      //}
  }

  void json_writer::enumeration(const std::size_t index, const epee::span<char const* const> enums)
  {
    if (enums.size() < index)
      throw std::logic_error{"Invalid enum/string value"};
    string({enums[index], std::strlen(enums[index])});
  }

  void json_writer::start_array(std::size_t)
  {
    formatter_.StartArray();
  }
  void json_writer::end_array()
  {
    formatter_.EndArray();
  }

  void json_writer::start_object(std::size_t)
  {
    formatter_.StartObject();
  }
  void json_writer::key(const boost::string_ref str)
  {
    formatter_.Key(str.data(), str.size());
    check_flush();
  }
  void json_writer::key(const std::uintmax_t id)
  {
    auto str = json_writer::to_string(id);
    key(str.data());
  }
  void json_writer::key(unsigned, const boost::string_ref str)
  {
    key(str);
  }
  void json_writer::end_object()
  {
    formatter_.EndObject();
  }

  void json_stream_writer::do_flush(epee::span<const std::uint8_t> bytes)
  {
    dest.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
}
