// Copyright (c) 2021, The Monero Project
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
#include <iosfwd>

//! Expands to an object that tracks current source location
#define MLWS_CURRENT_LOCATION ::lws::source_location{__FILE__, __LINE__}

namespace lws
{
  //! Tracks source location in one object, with `std::ostream` output.
  class source_location
  {
    // NOTE This is available in newer Boost versions
    const char* file_;
    int line_;

  public:
    constexpr source_location() noexcept
      : file_(nullptr), line_(0)
    {}

    //! `file` must be in static memory
    constexpr source_location(const char* file, int line) noexcept
      : file_(file), line_(line)
    {}

    source_location(const source_location&) = default;
    source_location& operator=(const source_location&) = default;

    //! \return Possibly `nullptr`, otherwise full file path
    const char* file_name() const noexcept { return file_; }
    int line() const noexcept { return line_; }
  };

  //! Prints `loc.file_name() + ':' + loc.line()`
  std::ostream& operator<<(std::ostream& os, source_location loc);
}
