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

#include "rate_limiter.h"

#include <algorithm>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/thread/lock_guard.hpp>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <vector>

#include "config.h"
namespace net
{
  namespace
  {
    static unsigned calls_per_second_(const std::chrono::steady_clock::time_point now, const std::chrono::steady_clock::time_point start, const unsigned calls) noexcept
    {
      const auto divisor =
        std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
      return calls / ((0 < divisor) ? divisor : 1);
    }
  }

  unsigned rate_limiter::calls_per_second(const std::chrono::steady_clock::time_point now) noexcept
  {
    return calls_per_second_(now, start_, calls_);
  }

  unsigned rate_limiter::adjust_window(const std::chrono::steady_clock::time_point now) noexcept
  {
    /* make sure the # of calls decreases each adjustment period;
      duration_cast is iffy with asio timers. */
    calls_ = std::max(calls_per_second(now), unsigned(1)) - 1;
    start_ = now;
    return calls_;
  }

  void rate_limiter::do_add_calls(std::shared_ptr<rate_limiter> more)
  {
    const auto now = std::chrono::steady_clock::now();
    const boost::lock_guard<boost::mutex> lock{sync_};
    if (merged.insert(more).second)
    {
      adjust_window(now);
      const boost::lock_guard<boost::mutex> lock2{more->sync_};
      more->adjust_window(now);
      if (std::numeric_limits<unsigned>::max() - calls_ < more->calls_)
        calls_ = std::numeric_limits<unsigned>::max();
      else
        calls_ += more->calls_;
    }
  }

  struct rate_limiter::window
  {
    boost::asio::io_context::strand strand_;
    boost::asio::steady_timer timer_;
    std::vector<std::shared_ptr<rate_limiter>> active_;

    explicit window(boost::asio::io_context& io)
      : strand_(io), timer_(io), active_()
    {}

    static bool run(rate_limiter& self)
    {
      const auto now = std::chrono::steady_clock::now();
      const boost::lock_guard<boost::mutex> lock{self.sync_};
      return self.adjust_window(now);
    }

    struct loop
    {
      std::shared_ptr<window> self_;

      void operator()(const boost::system::error_code error = {})
      {
        if (!self_ || error == boost::asio::error::operation_aborted)
          throw std::logic_error{"loop::operator() failure"};

        window& self = *self_;
        auto end = self.active_.end();
        for (auto rate = self.active_.begin(); rate != end; )
        {
          if (!window::run(**rate))
          {
            /* Rate limiting == 0, so check to see if any connections are
              keeping this rate limiting open. */
            const std::weak_ptr<rate_limiter> check = *rate;
            rate->reset();
            *rate = check.lock();
            if (!(*rate)) // all connections cleared out
            {
              std::swap(*rate, *(end - 1));
              --end;
            }
            else
              ++rate;
          }
          else
            ++rate;
        }
        self.active_.erase(end, self.active_.end());

        // duration_cast + division works better if its at least a 3 sec window
        static_assert(std::chrono::seconds{3} <= lws::config::rate::max_calls_window);
        self.timer_.expires_after(lws::config::rate::max_calls_window);
        self.timer_.async_wait(boost::asio::bind_executor(self.strand_, std::move(*this)));
      }
    };
  };

  rate_limiter::rate_limiter()
    : start_(std::chrono::steady_clock::now()), merged(), calls_(0), sync_()
  {}

  std::shared_ptr<rate_limiter::window> rate_limiter::make_tracker(boost::asio::io_context& io)
  {
    auto tracker = std::make_shared<window>(io);
    window::loop{tracker}();
    return tracker;
  }

  void rate_limiter::track(std::shared_ptr<window> tracker, std::shared_ptr<rate_limiter> self)
  {
    if (!tracker || !self)
      throw std::logic_error{"nullptr given to rate_limiter::track"};
    boost::asio::io_context::strand& strand = tracker->strand_;
    boost::asio::post(strand, [tracker = std::move(tracker), self = std::move(self)] {
      tracker->active_.push_back(std::move(self));
    });
  }

  bool rate_limiter::rate_limited(const unsigned max_calls, const unsigned weight)
  {
    unsigned calls = 0;
    std::chrono::steady_clock::time_point start{};
    const auto now = std::chrono::steady_clock::now();

    {
      const boost::lock_guard<boost::mutex> lock{sync_};
      calls = calls_;
      if (std::numeric_limits<unsigned>::max() - calls_ < weight)
      {
        calls_ = std::numeric_limits<unsigned>::max();
        calls = adjust_window(now);
      }
      else
        calls_ += weight;
      start = start_;
    }

    return max_calls < calls_per_second_(now, start, calls);
  }

} // net
