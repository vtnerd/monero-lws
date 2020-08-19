// Copyright (c) 2018-2020, The Monero Project
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

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <stdexcept>
#include <string>

#include "common/command_line.h" // monero/src
#include "common/util.h"         // monero/src

namespace lws
{
  constexpr const char default_db_subdir[] = "/light_wallet_server";

  struct options
  {
    const command_line::arg_descriptor<std::string> db_path;
    const command_line::arg_descriptor<std::string> network;

    options()
      : db_path{"db-path", "Folder for LMDB files", tools::get_default_data_dir() + default_db_subdir}
      , network{"network", "<\"main\"|\"stage\"|\"test\"> - Blockchain net type", "main"}
    {}

    void prepare(boost::program_options::options_description& description) const
    {
      command_line::add_arg(description, db_path);
      command_line::add_arg(description, network);
      command_line::add_arg(description, command_line::arg_help);
    }

    void set_network(boost::program_options::variables_map const& args) const
    {
      const std::string net = command_line::get_arg(args, network);
      if (net == "main")
        lws::config::network = cryptonote::MAINNET;
      else if (net == "stage")
        lws::config::network = cryptonote::STAGENET;
      else if (net == "test")
        lws::config::network = cryptonote::TESTNET;
      else
        throw std::runtime_error{"Bad --network value"};
    }
  };
}
