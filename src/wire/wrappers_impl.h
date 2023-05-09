// Copyright (c) 2022, The Monero Project
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

#include <boost/range/adaptor/transformed.hpp>
#include "wire/error.h"
#include "wire/read.h"
#include "wire/write.h"
#include "wire/wrapper/array.h"

namespace wire
{
  //
  // free functions for `array_` wrapper
  //

  template<typename R, typename T, typename C>
  inline void read_bytes(R& source, array_<T, C> wrapper)
  {
    // see constraints directly above `array_` definition
    static_assert(std::is_same<R, void>::value, "array_ must have a read constraint for memory purposes");
    wire_read::array(source, wrapper.get_read_object());
  }

  template<typename W, typename T, typename C>
  inline void write_bytes(W& dest, const array_<T, C>& wrapper)
  {
    wire_write::array(dest, wrapper.get_container());
  }
  template<typename W, typename T, typename C, typename D>
  inline void write_bytes(W& dest, const array_<array_<T, C>, D>& wrapper)
  {
    using inner_type = typename array_<array_<T, C>, D>::inner_array_const;
    const auto wrap = [](const auto& val) -> inner_type { return {std::ref(val)}; };
    wire_write::array(dest, boost::adaptors::transform(wrapper.get_container(), wrap));
  }
} // wire
