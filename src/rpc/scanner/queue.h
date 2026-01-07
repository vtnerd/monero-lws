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

#include <algorithm>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>
#include <cstddef>
#include <optional>
#include <vector>

#include "db/fwd.h"
#include "rpc/scanner/commands.h"

namespace lws { namespace rpc { namespace scanner
{
  //! Notifies worker thread of new accounts to scan. All functions thread-safe.
  class queue
  {
  public:
    //! Status of upstream scan requests.
    struct status
    {
      std::optional<std::vector<lws::account>> replace; //!< Empty optional means replace **not** requested.
      std::vector<lws::account> push;
      std::size_t user_count;
    };

  private:
    std::optional<std::vector<lws::account>> replace_;
    std::vector<lws::account> push_;
    std::size_t user_count_;
    db::block_id current_min_height_;  //!< Minimum scan height of accounts on this thread
    boost::mutex sync_;
    boost::condition_variable poll_;
    bool stop_;

    status do_get_accounts();

  public: 
    queue();
    ~queue(); 

    //! `wait_for_accounts()` will return immediately, permanently.
    void stop();

    //! \return Total number of users given to this local thread
    std::size_t user_count();

    //! \return Current minimum scan height of accounts on this thread
    db::block_id current_min_height();

    //! Update the current minimum scan height for this thread
    void update_min_height(db::block_id height);

    //! \return Replacement and "push" accounts
    status get_accounts();

    //! Blocks until replace or push accounts become available OR `stop()` is called. 
    status wait_for_accounts();

    //! Replace existing accounts on thread with new `users`
    void replace_accounts(std::vector<lws::account> users);

    //! Push/add new accounts to scan on thread
    template<typename T>
    void push_accounts(T begin, T end)
    {
      {
        const boost::lock_guard<boost::mutex> lock{sync_};
        user_count_ += (end - begin);
        // Update min height if any new account has a lower scan height
        for (auto it = begin; it != end; ++it)
        {
          if (current_min_height_ == db::block_id(0) || it->scan_height() < current_min_height_)
            current_min_height_ = it->scan_height();
        }
        push_.insert(push_.end(), std::make_move_iterator(begin), std::make_move_iterator(end));
      }
      poll_.notify_all();
    }
  };
}}} // lws // rpc // scanner
