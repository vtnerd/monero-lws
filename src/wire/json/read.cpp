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

#include "read.h"

#include <algorithm>
#include <limits>
#include <rapidjson/memorystream.h>
#include <stdexcept>

#include "common/expect.h" // monero/src
#include "hex.h"           // monero/contrib/epee/include
#include "wire/error.h"
#include "wire/json/error.h"

namespace
{
  //! Maximum number of bytes to display "near" JSON error.
  constexpr const std::size_t snippet_size = 30;

  struct json_default_reject : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, json_default_reject>
  {
    bool Default() const noexcept { return false; }
  };

  //! \throw std::system_error by converting `code` into a std::error_code
  [[noreturn]] void throw_json_error(const epee::span<char> source, const rapidjson::Reader& reader, const wire::error::schema expected)
  {
    const std::size_t offset = std::min(source.size(), reader.GetErrorOffset());
    const std::size_t start = offset;//std::max(snippet_size / 2, offset) - (snippet_size / 2);
    const std::size_t end = start + std::min(snippet_size, source.size() - start);

    const boost::string_ref text{source.data() + start, end - start};
    const rapidjson::ParseErrorCode parse_error = reader.GetParseErrorCode();
    switch (parse_error)
    {
    default:
      WIRE_DLOG_THROW(wire::error::rapidjson_e(parse_error), "near \"" << text << '"');
    case rapidjson::kParseErrorNone:
    case rapidjson::kParseErrorTermination: // the handler returned false
      break;
    }
    WIRE_DLOG_THROW(expected, "near '" << text << '\'');
  }
}

namespace wire
{
  struct json_reader::rapidjson_sax
  {
    struct string_contents
    {
       const char* ptr;
       std::size_t length;
    };

    union
    {
      bool boolean;
      std::intmax_t integer;
      std::uintmax_t unsigned_integer;
      double number;
      string_contents string;
    } value;

    error::schema expected_;
    bool negative;

    explicit rapidjson_sax(error::schema expected) noexcept
      : expected_(expected), negative(false)
    {}

    bool Null() const noexcept
    {
      return expected_ == error::schema::none;
    }

    bool Bool(bool i) noexcept
    {
      value.boolean = i;
      return expected_ == error::schema::boolean || expected_ == error::schema::none;
    }

    bool Int(int i) noexcept
    {
      return Int64(i);
    }
    bool Uint(unsigned i) noexcept
    {
      return Uint64(i);
    }
    bool Int64(std::int64_t i) noexcept
    {
      negative = true;
      switch(expected_)
      {
      default:
        return false;
      case error::schema::integer:
        value.integer = i;
        break;
      case error::schema::number:
        value.number = i;
        break;
      case error::schema::none:
        break;
      }
      return true;
    }
    bool Uint64(std::uint64_t i) noexcept
    {
      switch (expected_)
      {
      default:
        return false;
      case error::schema::integer:
        value.unsigned_integer = i;
        break;
      case error::schema::number:
        value.number = i;
        break;
      case error::schema::none:
        break;
      }
      return true;
    }

    bool Double(double i) noexcept
    {
      value.number = i;
      return expected_ == error::schema::number || expected_ == error::schema::none;
    }

    bool RawNumber(const char*, std::size_t, bool) const noexcept
    {
      return false;
    }

    bool String(const char* str, std::size_t length, bool) noexcept
    {
      value.string = {str, length};
      return expected_ == error::schema::string || expected_ == error::schema::none;
    }
    bool Key(const char* str, std::size_t length, bool)
    {
      return String(str, length, true);
    }

    bool StartArray() const noexcept { return expected_ == error::schema::none; }
    bool EndArray(std::size_t) const noexcept { return expected_ == error::schema::none; }
    bool StartObject() const noexcept { return expected_ == error::schema::none; }
    bool EndObject(std::size_t) const noexcept { return expected_ == error::schema::none; }
  };

  void json_reader::read_next_value(rapidjson_sax& handler)
  {
    rapidjson::InsituStringStream stream{current_.data()};
    if (!reader_.Parse<rapidjson::kParseStopWhenDoneFlag>(stream, handler))
      throw_json_error(current_, reader_, handler.expected_);
    current_.remove_prefix(stream.Tell());
  }

  char json_reader::get_next_token()
  {
    rapidjson::InsituStringStream stream{current_.data()};
    rapidjson::SkipWhitespace(stream);
    current_.remove_prefix(stream.Tell());
    return stream.Peek();
  }

  boost::string_ref json_reader::get_next_string()
  {
    if (get_next_token() != '"')
      WIRE_DLOG_THROW_(error::schema::string);
    current_.remove_prefix(1);

    void const* const end = std::memchr(current_.data(), '"', current_.size());
    if (!end)
      WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorStringMissQuotationMark));

    char const* const begin = current_.data();
    const std::size_t length = current_.remove_prefix(static_cast<const char*>(end) - current_.data() + 1);
    return {begin, length - 1};
  }

  void json_reader::skip_value()
  {
    rapidjson_sax accept_all{error::schema::none};
    read_next_value(accept_all);
  }

  json_reader::json_reader(std::string&& source)
    : reader(),
      source_(std::move(source)),
      current_(std::addressof(source_[0]), source_.size()),
      reader_()
  {}

  void json_reader::check_complete() const
  {
    if (depth())
      WIRE_DLOG_THROW(error::rapidjson_e(rapidjson::kParseErrorUnspecificSyntaxError), "Unexpected end");
  }

  bool json_reader::boolean()
  {
    rapidjson_sax json_bool{error::schema::boolean};
    read_next_value(json_bool);
    return json_bool.value.boolean;
  }

  std::intmax_t json_reader::integer()
  {
    rapidjson_sax json_int{error::schema::integer};
    read_next_value(json_int);
    if (json_int.negative)
      return json_int.value.integer;
    return integer::convert_to<std::intmax_t>(json_int.value.unsigned_integer);
  }

  std::uintmax_t json_reader::unsigned_integer()
  {
    rapidjson_sax json_uint{error::schema::integer};
    read_next_value(json_uint);
    if (!json_uint.negative)
      return json_uint.value.unsigned_integer;
    return integer::convert_to<std::uintmax_t>(json_uint.value.integer);
  }
    /*
  const std::vector<std::uintmax_t>& json_reader::unsigned_integer_array()
  {
      read_next_unsigned_array(
      }*/

  std::uintmax_t json_reader::safe_unsigned_integer()
  {
    if (get_next_token() != '"')
      WIRE_DLOG_THROW_(error::schema::string);
    current_.remove_prefix(1);

    const std::uintmax_t out = unsigned_integer();

    if (get_next_token() != '"')
      WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorStringMissQuotationMark));
    current_.remove_prefix(1);

    return out;
  }

  double json_reader::real()
  {
    rapidjson_sax json_number{error::schema::number};
    read_next_value(json_number);
    return json_number.value.number;
  }

  std::string json_reader::string()
  {
    rapidjson_sax json_string{error::schema::string};
    read_next_value(json_string);
    return std::string{json_string.value.string.ptr, json_string.value.string.length};
  }

  std::vector<std::uint8_t> json_reader::binary()
  {
    const boost::string_ref value = get_next_string();

    std::vector<std::uint8_t> out;
    out.resize(value.size() / 2);

    if (!epee::from_hex::to_buffer(epee::to_mut_span(out), value))
      WIRE_DLOG_THROW_(error::schema::binary);

    return out;
  }

  void json_reader::binary(epee::span<std::uint8_t> dest)
  {
    const boost::string_ref value = get_next_string();
    if (!epee::from_hex::to_buffer(dest, value))
      WIRE_DLOG_THROW(error::schema::fixed_binary, "of size" << dest.size() * 2 << " but got " << value.size());
  }

  std::size_t json_reader::enumeration(epee::span<char const* const> enums)
  {
    rapidjson_sax json_enum{error::schema::string};
    read_next_value(json_enum);

    const boost::string_ref value{json_enum.value.string.ptr, json_enum.value.string.length};
    for (std::size_t i = 0; i < enums.size(); ++i)
    {
      if (value == enums[i])
        return i;
    }

    WIRE_DLOG_THROW(error::schema::enumeration, value << " is not a valid enum");
    return enums.size();
  }

  std::size_t json_reader::start_array()
  {
    if (get_next_token() != '[')
      WIRE_DLOG_THROW_(error::schema::array);
    current_.remove_prefix(1);
    increment_depth();
    return 0;
  }

  bool json_reader::is_array_end(const std::size_t count)
  {
    const char next = get_next_token();
    if (next == 0)
      WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorArrayMissCommaOrSquareBracket));
    if (next == ']')
    {
      current_.remove_prefix(1);
      return true;
    }

    if (count)
    {
      if (next != ',')
        WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorArrayMissCommaOrSquareBracket));
      current_.remove_prefix(1);
    }
    return false;
  }

  std::size_t json_reader::start_object()
  {
    if (get_next_token() != '{')
      WIRE_DLOG_THROW_(error::schema::object);
    current_.remove_prefix(1);
    increment_depth();
    return 0;
  }

  bool json_reader::key(const epee::span<const key_map> map, std::size_t& state, std::size_t& index)
  {
    rapidjson_sax json_key{error::schema::string};
    const auto process_key = [map] (const rapidjson_sax::string_contents value)
    {
      const boost::string_ref key{value.ptr, value.length};
      for (std::size_t i = 0; i < map.size(); ++i)
      {
        if (map[i].name == key)
          return i;
      }
      return map.size();
    };

    index = map.size();
    for (;;)
    {
      // check for object or text end
      const char next = get_next_token();
      if (next == 0)
        WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorObjectMissCommaOrCurlyBracket));
      if (next == '}')
      {
        current_.remove_prefix(1);
        return false;
      }

      // parse next field token
      if (state)
      {
        if (next != ',')
          WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorObjectMissCommaOrCurlyBracket));
        current_.remove_prefix(1);
      }
      ++state;

      // parse key
      read_next_value(json_key);
      index = process_key(json_key.value.string);
      if (get_next_token() != ':')
        WIRE_DLOG_THROW_(error::rapidjson_e(rapidjson::kParseErrorObjectMissColon));
      current_.remove_prefix(1);

      // parse value
      if (index != map.size())
        break;
      skip_value();
    }
    return true;
  }
}

