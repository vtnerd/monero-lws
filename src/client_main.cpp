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

#include <boost/asio/signal_set.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/thread/thread.hpp>
#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/command_line.h" // monero/src/
#include "common/expect.h"       // monero/src/
#include "common/util.h"         // monero/src/
#include "config.h"
#include "cryptonote_config.h"   // monero/src/
#include "db/storage.h"
#include "error.h"
#include "options.h"
#include "misc_log_ex.h"         // monero/contrib/epee/include
#include "rpc/scanner/client.h"
#include "rpc/scanner/write_commands.h"
#include "scanner.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"

namespace
{
  struct options
  {
    const command_line::arg_descriptor<std::string> config_file;
    const command_line::arg_descriptor<unsigned short> log_level;
    const command_line::arg_descriptor<std::string> lws_daemon;
    const command_line::arg_descriptor<std::string> lws_pass;
    const command_line::arg_descriptor<std::string> monerod_rpc;
    const command_line::arg_descriptor<std::string> monerod_sub;
    const command_line::arg_descriptor<std::string> network;
    const command_line::arg_descriptor<std::size_t> scan_threads;

    static std::string get_default_zmq()
    {
      static constexpr const char base[] = "tcp://127.0.0.1:";
      switch (lws::config::network)
      {
      case cryptonote::TESTNET:
        return base + std::to_string(config::testnet::ZMQ_RPC_DEFAULT_PORT);
      case cryptonote::STAGENET:
        return base + std::to_string(config::stagenet::ZMQ_RPC_DEFAULT_PORT);
      case cryptonote::MAINNET:
      default:
        break;
      }
      return base + std::to_string(config::ZMQ_RPC_DEFAULT_PORT);
    }

    options()
      : config_file{"config-file", "Specify any option in a config file; <name>=<value> on separate lines"}
      , log_level{"log-level", "Log level [0-4]", 1}
      , lws_daemon{"lws-daemon", "Specify monero-lws-daemon main process <[ip:]port>", ""}
      , lws_pass{"lws-pass", "Specify monero-lws-daemon password", ""}
      , monerod_rpc{"monerod-rpc", "Specify monero ZMQ RPC server <tcp://ip:port> or <ipc:///path>", get_default_zmq()}
      , monerod_sub{"monerod-sub", "Specify monero ZMQ RPC server <tcp://ip:port> or <ipc:///path>", ""}
      , network{"network", "<\"main\"|\"stage\"|\"test\"> - Blockchain net type", "main"}
      , scan_threads{"scan-threads", "Number of scan threads", boost::thread::hardware_concurrency()}
    {}

    void prepare(boost::program_options::options_description& description) const
    {
      command_line::add_arg(description, config_file);
      command_line::add_arg(description, log_level);
      command_line::add_arg(description, lws_daemon);
      command_line::add_arg(description, lws_pass);
      command_line::add_arg(description, monerod_rpc);
      command_line::add_arg(description, monerod_sub);
      command_line::add_arg(description, network);
      command_line::add_arg(description, scan_threads);
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

  struct program
  {
    std::string lws_daemon;
    std::string lws_pass;
    std::string monerod_rpc;
    std::string monerod_sub;
    std::size_t scan_threads;    
  };

  void print_help(std::ostream& out)
  {
    boost::program_options::options_description description{"Options"};
    options{}.prepare(description);

    out << "Usage: [options]" << std::endl;
    out << description;
  }

  std::optional<program> get_program(int argc, char** argv)
  {
    namespace po = boost::program_options;

    const options opts{};
    po::variables_map args{};
    {
      po::options_description description{"Options"};
      opts.prepare(description);

      po::store(
        po::command_line_parser(argc, argv).options(description).run(), args
      );
      po::notify(args);

      if (!command_line::is_arg_defaulted(args, opts.config_file))
      {
        boost::filesystem::path config_path{command_line::get_arg(args, opts.config_file)};
        if (!boost::filesystem::exists(config_path))
          MONERO_THROW(lws::error::configuration, "Config file does not exist");

        po::store(
          po::parse_config_file<char>(config_path.string<std::string>().c_str(), description), args
        );
        po::notify(args);
      }
    }

    if (command_line::get_arg(args, command_line::arg_help))
    {
      print_help(std::cout);
      return std::nullopt;
    }

    opts.set_network(args); // do this first, sets global variable :/
    mlog_set_log_level(command_line::get_arg(args, opts.log_level));

    program prog{
      command_line::get_arg(args, opts.lws_daemon),
      command_line::get_arg(args, opts.lws_pass),
      command_line::get_arg(args, opts.monerod_rpc),
      command_line::get_arg(args, opts.monerod_sub),
      command_line::get_arg(args, opts.scan_threads)
    };
    prog.scan_threads = std::max(std::size_t(1), prog.scan_threads);

    if (command_line::is_arg_defaulted(args, opts.monerod_rpc))
      prog.monerod_rpc = options::get_default_zmq();

    return prog;
  }

  struct send_users
  {
    std::shared_ptr<lws::rpc::scanner::client> client_;

    bool operator()(boost::asio::io_context&, lws::rpc::client&, net::http::client&, epee::span<const crypto::hash> chain, epee::span<const lws::account> users, epee::span<const lws::db::pow_sync> pow)
    {
      if (!client_)
        return false;
      if (users.empty())
        return true;
      if (!pow.empty() || chain.empty())
        return false;

      std::vector<crypto::hash> chain_copy{};
      chain_copy.reserve(chain.size());
      std::copy(chain.begin(), chain.end(), std::back_inserter(chain_copy));

      std::vector<lws::account> users_copy{};
      users_copy.reserve(users.size());
      for (const auto& user : users)
        users_copy.push_back(user.clone());

      MINFO("Processed " << chain.size() << " block(s) against " << users.size() << " account(s)");
      lws::rpc::scanner::client::send_update(client_, std::move(users_copy), std::move(chain_copy));
      return true;
    }
  };

  void run_thread(lws::scanner_sync& self, std::shared_ptr<lws::rpc::scanner::client> client, lws::rpc::client& zclient, std::shared_ptr<lws::rpc::scanner::queue> queue)
  {
    struct stop_
    {
      lws::scanner_sync& self;
      ~stop_() { self.stop(); }
    } stop{self};

    if (!client || !queue)
      return;

    try
    {
      while (self.is_running())
      {
        std::vector<lws::account> users{};
        auto status = queue->wait_for_accounts();
        if (status.replace)
          users = std::move(*status.replace);
        users.insert(
          users.end(),
          std::make_move_iterator(status.push.begin()),
          std::make_move_iterator(status.push.end())
        );

        if (!users.empty())
        {
          static constexpr const lws::scanner_options opts{0, 0, lws::MINIMUM_BLOCK_DEPTH, 0, false, false, false, false};

          auto new_client = MONERO_UNWRAP(zclient.clone());
          MONERO_UNWRAP(new_client.watch_scan_signals());
          send_users send{client};
          if (!lws::scanner::loop(self, std::move(send), std::nullopt, std::move(new_client), std::move(users), *queue, opts, false))
            return;
        }
      }
    }
    catch (const std::exception& e)
    {
      self.shutdown();
      MERROR("Client shutdown with error " << e.what());
    }
    catch (...)
    {
      self.shutdown();
      MERROR("Unknown error");
    }
  }

  void run(program prog)
  {
    MINFO("Using monerod ZMQ RPC at " << prog.monerod_rpc);
    auto ctx = lws::rpc::context::make(std::move(prog.monerod_rpc), std::move(prog.monerod_sub), {}, {}, std::chrono::minutes{0}, false);

    lws::scanner_sync self{epee::net_utils::ssl_verification_t::system_ca};

    /*! \NOTE `ctx will need a strand or lock if multiple threads use
      `self.io.run()` in the future. */

    boost::asio::signal_set signals{self.io_};
    signals.add(SIGINT);
    signals.async_wait([&self] (const boost::system::error_code& error, int)
    {
      if (error != boost::asio::error::operation_aborted)
        self.shutdown();
    });

    for (;;)
    {
      if (self.stop_ && self.is_running())
        boost::this_thread::sleep_for(boost::chrono::seconds{1});

      self.stop_ = false;
      self.io_.restart();
      if (self.has_shutdown())
        return;

      std::vector<lws::rpc::client> zclients;
      zclients.reserve(prog.scan_threads);

      std::vector<std::shared_ptr<lws::rpc::scanner::queue>> queues{};
      queues.resize(prog.scan_threads);

      for (auto& queue : queues)
      {
        queue = std::make_shared<lws::rpc::scanner::queue>();
        zclients.push_back(MONERO_UNWRAP(ctx.connect()));
      }

      auto client = std::make_shared<lws::rpc::scanner::client>(
        self.io_, prog.lws_daemon, prog.lws_pass, queues
      );
      lws::rpc::scanner::client::connect(client);

      std::vector<boost::thread> threads{};
      threads.reserve(prog.scan_threads);

      struct stop_
      {
        lws::scanner_sync& self;
        lws::rpc::context& ctx;
        std::vector<std::shared_ptr<lws::rpc::scanner::queue>> queues;
        std::vector<boost::thread>& threads;

        ~stop_()
        {
          self.stop();
          if (self.has_shutdown())
            ctx.raise_abort_process();
          else
            ctx.raise_abort_scan();
          for (const auto& queue : queues)
          {
            if (queue)
              queue->stop();
          }
          for (auto& thread : threads)
            thread.join();
        }
      } stop{self, ctx, queues, threads};

      boost::thread::attributes attrs;
      attrs.set_stack_size(THREAD_STACK_SIZE);

      for (std::size_t i = 0; i < prog.scan_threads; ++i)
        threads.emplace_back(attrs, std::bind(&run_thread, std::ref(self), client, std::ref(zclients[i]), queues[i]));

      self.io_.run();
    } // while scanner running
  }
} // anonymous

int main(int argc, char** argv)
{
  tools::on_startup(); // if it throws, don't use MERROR just print default msg

  try
  {
    std::optional<program> prog;

    try
    {
      prog = get_program(argc, argv);
    }
    catch (std::exception const& e)
    {
      std::cerr << e.what() << std::endl << std::endl;
      print_help(std::cerr);
      return EXIT_FAILURE;
    }

    if (prog)
      run(std::move(*prog));
  }
  catch (std::exception const& e)
  {
    MERROR("Client shutdown with error " << e.what());
    return EXIT_FAILURE;
  }
  catch (...)
  {
    MERROR("Unknown exception");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;

}
