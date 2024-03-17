// Copyright (c) 2024, The Monero Project
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

#include <cstddef>
#include <limits>
#include "wire/traits.h"
#include "wire/read.h"
#include "wire/write.h"

namespace wire
{
  //! \brief Wrapper that removes read constraints
  template<typename T>
  struct trusted_array_
  {
    using container_type = wire::unwrap_reference_t<T>;
    T container;

    const container_type& get_container() const noexcept { return container; }
    container_type& get_container() noexcept { return container; }

    // concept requirements for optional fields

    explicit operator bool() const noexcept { return !get_container().empty(); }
    trusted_array_& emplace() noexcept { return *this; }

    trusted_array_& operator*() noexcept { return *this; }
    const trusted_array_& operator*() const noexcept { return *this; }

    void reset() { get_container().clear(); }
  };

  template<typename T>
  trusted_array_<T> trusted_array(T value)
  {
    return {std::move(value)};
  }

  template<typename R, typename T>
  void read_bytes(R& source, trusted_array_<T> dest)
  {
    wire_read::array_unchecked(source, dest.get_container(), 0, std::numeric_limits<std::size_t>::max());
  }

  template<typename W, typename T>
  void write_bytes(W& dest, const trusted_array_<T> source)
  {
    wire_write::array(dest, source.get_container());
  }
}
