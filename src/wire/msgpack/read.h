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

#pragma once

#include <boost/utility/string_ref.hpp>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wire/field.h"
#include "wire/msgpack/base.h"
#include "wire/read.h"
#include "wire/traits.h"

namespace wire
{
  //! Reads MSGPACK tokens one-at-a-time for DOMless parsing
  class msgpack_reader : public reader
  {
    epee::byte_slice source_;
    std::size_t tags_remaining_; //!< Expected number of elements remaining

    //! \throw wire::exception with `error::msgpack::underflow_tree` 
    [[noreturn]] void throw_wire_exception();
    //! Decrement tags_remaining_ if not zero, \throw std::logic_error when `tags_remaining_ == 0`.
    void update_tags_remaining()
    {
      if (tags_remaining_)
        --tags_remaining_;
      else
        throw_wire_exception();
    }

    //! Skips next value. \throw wire::exception if invalid JSON syntax.
    void skip_value();

    //! \return Next tag but leave `remaining_` untouched.
    msgpack::tag peek_tag();
    //! \return Next tag and remove first byte from `remaining_`.
    msgpack::tag get_tag();

    //! \return Integer from `remaining_` where positive fixed tag has been checked.
    std::intmax_t do_integer(msgpack::tag);
    //! \return Integer from `remaining_` where fixed tag has been checked.
    std::uintmax_t do_unsigned_integer(msgpack::tag);

    //! \return Number of items determined by `T` fixed tag and `U` tuple of tags.
    template<typename T, typename U>
    std::size_t read_count(error::schema);

  public:
    explicit msgpack_reader(epee::byte_slice&& source)
      : reader(nullptr), source_(std::move(source)), tags_remaining_(1)
    {
      remaining_ = {source_.data(), source_.size()};
    }

    //! \throw wire::exception if JSON parsing is incomplete.
    void check_complete() const override final;

    //! \throw wire::exception if next token not a boolean.
    bool boolean() override final;

    //! \throw wire::expception if next token not an integer.
    std::intmax_t integer() override final
    {
      update_tags_remaining();
      const msgpack::tag next = get_tag();
      if (std::uint8_t(next) <= msgpack::ftag_unsigned::max())
        return std::uint8_t(next);
      return do_integer(next);
    }

    //! \throw wire::exception if next token not an unsigned integer.
    std::uintmax_t unsigned_integer() override final
    {
      update_tags_remaining();
      const msgpack::tag next = get_tag();
      if (std::uint8_t(next) <= msgpack::ftag_unsigned::max())
        return std::uint8_t(next);
      return do_unsigned_integer(next);
    }

    //! \throw wire::exception if next token not a valid real number
    double real() override final;

    //! \throw wire::exception if next token not a string
    std::string string() override final;

    //! \throw wire::exception if next token cannot be read as hex
    std::vector<std::uint8_t> binary() override final;

    //! \throw wire::exception if next token cannot be read as hex into `dest`.
    void binary(epee::span<std::uint8_t> dest) override final;


    //! \throw wire::exception if next token not `[`.
    std::size_t start_array(std::size_t min_element_size) override final;

    //! \return true when `count == 0`.
    bool is_array_end(const std::size_t count) override final;


    //! \throw wire::exception if next token not `{`.
    std::size_t start_object() override final;

    /*! \throw wire::exception if next token not key or `}`.
        \param[out] index of key match within `map`.
        \return True if another value to read. */
    bool key(epee::span<const key_map> map, std::size_t&, std::size_t& index) override final;
  };
} // wire
