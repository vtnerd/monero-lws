#pragma once

#include <cstdint>
#include "cryptonote_config.h"

namespace lws
{
namespace config
{
  extern cryptonote::network_type network;

  //! Max queued clients when waiting for cacheable daemon response
  constexpr const std::size_t daemon_wait_queue_max = 25000;

  //! Max queued clients for `get_random_outs`
  constexpr const std::size_t get_random_outs_max = 10000;

  //! Max memory allowed for `submit_raw_tx` buffering
  constexpr const std::size_t submit_tx_max = 100 * 1024 * 1024;
}
}
