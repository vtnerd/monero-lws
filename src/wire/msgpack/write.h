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

#include <array>
#include <boost/utility/string_ref.hpp>
#include <cstdint>
#include <iosfwd>
#include <limits>

#include "byte_stream.h" // monero/contrib/epee/include
#include "span.h"        // monero/contrib/epee/include
#include "wire/field.h"
#include "wire/filters.h"
#include "wire/msgpack/base.h"
#include "wire/traits.h"
#include "wire/write.h"

namespace wire
{
  //! Writes MSGPACK tokens one-at-a-time for DOMless output.
  class msgpack_writer : public writer
  {
    epee::byte_stream bytes_;
    std::size_t expected_; //! Tracks number of expected elements remaining
    const bool integer_keys_;
    bool needs_flush_;

    //! Provided data currently in `bytes_`.
    virtual void do_flush(epee::span<const uint8_t>);

    //! Flush written bytes to `do_flush(...)` if configured
    void check_flush();
    
    void do_integer(std::intmax_t);
    void do_unsigned_integer(std::uintmax_t);
    
    template<typename T>
    void integer_t(const T value)
    {
      if (0 <= value)
      {
        if (value <= msgpack::ftag_unsigned::max())
          bytes_.put(std::uint8_t(value));
        else // if multibyte
          do_unsigned_integer(std::uintmax_t(value));
      }
      else // if negative
        do_integer(value);
      --expected_;
    }

    template<typename T>
    void unsigned_integer_t(const T value)
    {
      if (value <= msgpack::ftag_unsigned::max())
        bytes_.put(std::uint8_t(value));
      else // if multibyte
        do_unsigned_integer(value);
      --expected_;
    }

  protected:
    msgpack_writer(epee::byte_stream&& initial, bool integer_keys, bool needs_flush)
      : writer(), bytes_(std::move(initial)), expected_(1), integer_keys_(integer_keys), needs_flush_(needs_flush)
    {}

    msgpack_writer(bool integer_keys, bool needs_flush)
      : msgpack_writer(epee::byte_stream{}, integer_keys, needs_flush)
    {}

    //! \throw std::logic_error if tree was not completed
    void check_complete();

    //! \throw std::logic_error if incomplete msgpack tree. \return msgpack bytes
    epee::byte_slice take_msgpack();

    //! Flush bytes in local buffer to `do_flush(...)`
    void flush()
    {
      do_flush({bytes_.data(), bytes_.size()});
      bytes_.clear();
    }

  public:
    msgpack_writer(const msgpack_writer&) = delete;
    virtual ~msgpack_writer() noexcept;
    msgpack_writer& operator=(const msgpack_writer&) = delete;

    void boolean(const bool value) override final
    {
      if (value)
        bytes_.put(std::uint8_t(msgpack::tag::True));
      else
        bytes_.put(std::uint8_t(msgpack::tag::False));
      --expected_;
    }

    void integer(const int value) override final
    { integer_t(value); }

    void integer(const std::intmax_t value) override final
    { integer_t(value); }

    void unsigned_integer(const unsigned value) override final
    { unsigned_integer_t(value); }

    void unsigned_integer(const std::uintmax_t value) override final
    { unsigned_integer_t(value); }

    void real(double) override final;

    //! \throw wire::exception if `source.size()` exceeds 2^32-1
    void string(boost::string_ref source) override final;
    //! \throw wire::exception if `source.size()` exceeds 2^32-1
    void binary(epee::span<const std::uint8_t> source) override final;

    void enumeration(std::size_t index, epee::span<char const* const> enums) override final;

    //! \throw wire::exception if `items` exceeds 2^32-1.
    void start_array(std::size_t items) override final;
    void end_array() override final { --expected_; }

    //! \throw wire::exception if `items` exceeds 2^32-1
    void start_object(std::size_t items) override final;
    void key(std::uintmax_t) override final;
    void key(boost::string_ref) override final;
    void key(unsigned, boost::string_ref) override final;
    void end_object() override final { --expected_; }
  };

  //! Buffers entire JSON message in memory
  struct msgpack_slice_writer final : msgpack_writer
  {
    msgpack_slice_writer(epee::byte_stream&& initial, bool integer_keys = false)
      : msgpack_writer(std::move(initial), integer_keys, false)
    {}

    explicit msgpack_slice_writer(bool integer_keys = false)
      : msgpack_writer(integer_keys, false)
    {}

    //! \throw std::logic_error if incomplete JSON tree \return JSON bytes
    epee::byte_slice take_bytes()
    {
      return msgpack_writer::take_msgpack();
    }
  };

  //! Periodically flushes JSON data to `std::ostream`
  class msgpack_stream_writer final : public msgpack_writer
  {
    std::ostream& dest;

    virtual void do_flush(epee::span<const std::uint8_t>) override final;
  public:
    explicit msgpack_stream_writer(std::ostream& dest, bool integer_keys = false)
      : msgpack_writer(integer_keys, true), dest(dest)
    {}

    //! Flush remaining bytes to stream \throw std::logic_error if incomplete JSON tree
    void finish()
    {
      check_complete();
      flush();
    }
  };

  template<typename T>
  epee::byte_slice msgpack::to_bytes(const T& source)
  {
    return wire_write::to_bytes<msgpack_slice_writer>(source);
  }

  template<typename T, typename F = identity_>
  inline void array(msgpack_writer& dest, const T& source, F filter = F{})
  {
    wire_write::array(dest, source, source.size(), std::move(filter));
  }
  template<typename T, typename F>
  inline void write_bytes(msgpack_writer& dest, as_array_<T, F> source)
  {
    wire::array(dest, source.get_value(), std::move(source.filter));
  }
  template<typename T>
  inline enable_if<is_array<T>::value> write_bytes(msgpack_writer& dest, const T& source)
  {
    wire::array(dest, source);
  }

  template<typename T, typename F = identity_, typename G = identity_>
  inline void dynamic_object(msgpack_writer& dest, const T& source, F key_filter = F{}, G value_filter = G{})
  {
    // works with "lazily" computed ranges
    wire_write::dynamic_object(dest, source, 0, std::move(key_filter), std::move(value_filter));
  }
  template<typename T, typename F, typename G>
  inline void write_bytes(msgpack_writer& dest, as_object_<T, F, G> source)
  {
    wire::dynamic_object(dest, source.get_map(), std::move(source.key_filter), std::move(source.value_filter));
  }

  template<typename... T>
  inline void object(msgpack_writer& dest, T... fields)
  {
    wire_write::object(dest, std::move(fields)...);
  }
}
