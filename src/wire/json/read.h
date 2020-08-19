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

#include <boost/utility/string_ref.hpp>
#include <cstddef>
#include <rapidjson/reader.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wire/field.h"
#include "wire/json/base.h"
#include "wire/read.h"
#include "wire/traits.h"

namespace wire
{
  //! Reads JSON tokens one-at-a-time for DOMless parsing
  class json_reader : public reader
  {
    struct rapidjson_sax;

    std::string source_;
    epee::span<char> current_;
    rapidjson::Reader reader_;

    void read_next_value(rapidjson_sax& handler);
    char get_next_token();
    boost::string_ref get_next_string();

    //! Skips next value. \throw wire::exception if invalid JSON syntax.
    void skip_value();

  public:
    explicit json_reader(std::string&& source);

    //! \throw wire::exception if JSON parsing is incomplete.
    void check_complete() const override final;

    //! \throw wire::exception if next token not a boolean.
    bool boolean() override final;

    //! \throw wire::expception if next token not an integer.
    std::intmax_t integer() override final;

    //! \throw wire::exception if next token not an unsigned integer.
    std::uintmax_t unsigned_integer() override final;

    //! \throw wire::exception if next token is not an integer encoded as string
    std::uintmax_t safe_unsigned_integer();

    //! \throw wire::exception if next token not a valid real number
    double real() override final;

    //! \throw wire::exception if next token not a string
    std::string string() override final;

    //! \throw wire::exception if next token cannot be read as hex
    std::vector<std::uint8_t> binary() override final;

    //! \throw wire::exception if next token cannot be read as hex into `dest`.
    void binary(epee::span<std::uint8_t> dest) override final;

    //! \throw wire::exception if invalid next token invalid enum. \return Index in `enums`.
    std::size_t enumeration(epee::span<char const* const> enums) override final;


    //! \throw wire::exception if next token not `[`.
    std::size_t start_array() override final;

    //! Skips whitespace to next token. \return True if next token is eof or ']'.
    bool is_array_end(std::size_t count) override final;


    //! \throw wire::exception if next token not `{`.
    std::size_t start_object() override final;

    /*! \throw wire::exception if next token not key or `}`.
        \param[out] index of key match within `map`.
        \return True if another value to read. */
    bool key(epee::span<const key_map> map, std::size_t&, std::size_t& index) override final;
  };


  // Don't call `read` directly in this namespace, do it from `wire_read`.

  template<typename T>
  expect<T> json::from_bytes(std::string&& bytes)
  {
    json_reader source{std::move(bytes)};
    return wire_read::to<T>(source);
  }

  // specialization prevents type "downgrading" to base type in cpp files

  template<typename T>
  inline void array(json_reader& source, T& dest)
  {
    wire_read::array(source, dest);
  }

  template<typename... T>
  inline void object(json_reader& source, T... fields)
  {
    wire_read::object(source, wire_read::tracker<T>{std::move(fields)}...);
  }
} // wire
