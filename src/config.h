#pragma once

#include <chrono>
#include <cstdint>
#include "cryptonote_config.h"

namespace lws
{
namespace config
{
  extern cryptonote::network_type network;

  // All rate-limiting config options. See `docs/rate_limiting.md`
  namespace rate
  {
    //! Max (weighted) API calls per second (default). Zero == disabled at build
    constexpr const unsigned max_calls_per_second = 50;


    //! Weight for account creation rate limiting
    constexpr const unsigned account_creation_weight = 40;

    //! Max queued clients when waiting for cacheable daemon response
    constexpr const std::size_t daemon_wait_queue_max = 25000;

    //! Max queued clients for `get_random_outs`
    constexpr const std::size_t get_random_outs_max = 10000;

    //! Weight for ip/account rate limiting
    constexpr const unsigned get_random_outs_weight = 20;

    //! Calls per second window
    constexpr const std::chrono::seconds max_calls_window{5};

    //! Max memory allowed for `submit_raw_tx` buffering
    constexpr const std::size_t submit_tx_max = 100 * 1024 * 1024;
  }
}
}
