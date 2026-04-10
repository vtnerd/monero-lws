// Copyright (c) 2025, The Monero Project
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

#include <boost/asio/io_context.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/system/error_code.hpp>
#include <boost/thread/mutex.hpp>
#include <chrono>
#include <memory>
#include <utility>

namespace net
{
  //! Tracks weighted calls over a fixed time interval for rate limiting.
  class rate_limiter
  {
    std::chrono::steady_clock::time_point start_;
    boost::container::flat_set<std::weak_ptr<const rate_limiter>, std::owner_less<std::weak_ptr<const rate_limiter>>> merged;
    unsigned calls_;
    boost::mutex sync_;
 
    unsigned calls_per_second(const std::chrono::steady_clock::time_point now) noexcept;
    unsigned adjust_window(const std::chrono::steady_clock::time_point now) noexcept;
    void do_add_calls(std::shared_ptr<rate_limiter> more);

  public:
    struct window;

    explicit rate_limiter();

    static std::shared_ptr<window> make_tracker(boost::asio::io_context& io);
    static void track(std::shared_ptr<window> tracker, std::shared_ptr<rate_limiter> self);

    void add_calls(std::shared_ptr<rate_limiter> more)
    {
      if (more)
        do_add_calls(std::move(more));
    }
  
    bool rate_limited(unsigned max_calls, unsigned weight);
  };
} // net
