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

#include <boost/filesystem/operations.hpp>
#include <boost/optional/optional.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <iostream>
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
#include "rest_server.h"
#include "scanner.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"

namespace
{
  struct options : lws::options
  {
    const command_line::arg_descriptor<std::string> daemon_rpc;
    const command_line::arg_descriptor<std::string> daemon_sub;
    const command_line::arg_descriptor<std::string> zmq_pub;
#ifdef MLWS_RMQ_ENABLED
    const command_line::arg_descriptor<std::string> rmq_address;
    const command_line::arg_descriptor<std::string> rmq_credentials;
    const command_line::arg_descriptor<std::string> rmq_exchange;
    const command_line::arg_descriptor<std::string> rmq_routing;
#endif
    const command_line::arg_descriptor<std::vector<std::string>> rest_servers;
    const command_line::arg_descriptor<std::vector<std::string>> admin_rest_servers;
    const command_line::arg_descriptor<std::string> rest_ssl_key;
    const command_line::arg_descriptor<std::string> rest_ssl_cert;
    const command_line::arg_descriptor<std::size_t> rest_threads;
    const command_line::arg_descriptor<std::size_t> scan_threads;
    const command_line::arg_descriptor<std::vector<std::string>> access_controls;
    const command_line::arg_descriptor<bool> external_bind;
    const command_line::arg_descriptor<unsigned> create_queue_max;
    const command_line::arg_descriptor<std::chrono::minutes::rep> rates_interval;
    const command_line::arg_descriptor<unsigned short> log_level;
    const command_line::arg_descriptor<bool> disable_admin_auth;
    const command_line::arg_descriptor<std::string> webhook_ssl_verification;
    const command_line::arg_descriptor<std::string> config_file;
    const command_line::arg_descriptor<std::uint32_t> max_subaddresses;
    const command_line::arg_descriptor<bool> auto_accept_creation;
    const command_line::arg_descriptor<bool> untrusted_daemon;

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
      : lws::options()
      , daemon_rpc{"daemon", "<protocol>://<address>:<port> of a monerod ZMQ RPC", get_default_zmq()}
      , daemon_sub{"sub", "tcp://address:port or ipc://path of a monerod ZMQ Pub", ""}
      , zmq_pub{"zmq-pub", "tcp://address:port or ipc://path of a bind location for ZMQ pub events", ""}
#ifdef MLWS_RMQ_ENABLED
      , rmq_address{"rmq-address", "tcp://<host>/[vhost]"}
      , rmq_credentials{"rmq-credentials", "<user>:<pass>"}
      , rmq_exchange{"rmq-exchange", "Name of the RMQ exchange"}
      , rmq_routing{"rmq-routing", "Routing for the specified exchange"}
#endif
      , rest_servers{"rest-server", "[(https|http)://<address>:]<port>[/<prefix>] for incoming connections, multiple declarations allowed"}
      , admin_rest_servers{"admin-rest-server", "[(https|http])://<address>:]<port>[/<prefix>] for incoming admin connections, multiple declarations allowed"}
      , rest_ssl_key{"rest-ssl-key", "<path> to PEM formatted SSL key for https REST server", ""}
      , rest_ssl_cert{"rest-ssl-certificate", "<path> to PEM formatted SSL certificate (chains supported) for https REST server", ""}
      , rest_threads{"rest-threads", "Number of threads to process REST connections", 1}
      , scan_threads{"scan-threads", "Maximum number of threads for account scanning", boost::thread::hardware_concurrency()}
      , access_controls{"access-control-origin", "Specify a whitelisted HTTP control origin domain"}
      , external_bind{"confirm-external-bind", "Allow listening for external connections", false}
      , create_queue_max{"create-queue-max", "Set pending create account requests maximum", 10000}
      , rates_interval{"exchange-rate-interval", "Retrieve exchange rates in minute intervals from cryptocompare.com if greater than 0", 0}
      , log_level{"log-level", "Log level [0-4]", 1}
      , disable_admin_auth{"disable-admin-auth", "Make auth field optional in HTTP-REST requests", false}
      , webhook_ssl_verification{"webhook-ssl-verification", "[<none|system_ca>] specify SSL verification mode for webhooks", "system_ca"}
      , config_file{"config-file", "Specify any option in a config file; <name>=<value> on separate lines"}
      , max_subaddresses{"max-subaddresses", "Maximum number of subaddresses per primary account (defaults to 0)", 0}
      , auto_accept_creation{"auto-accept-creation", "New account creation requests are automatically accepted", false}
      , untrusted_daemon{"untrusted-daemon", "Perform (expensive) chain-verification and PoW checks", false}
    {}

    void prepare(boost::program_options::options_description& description) const
    {
      static constexpr const char rest_default[] = "https://0.0.0.0:8443";

      lws::options::prepare(description);
      command_line::add_arg(description, daemon_rpc);
      command_line::add_arg(description, daemon_sub);
      command_line::add_arg(description, zmq_pub);
#ifdef MLWS_RMQ_ENABLED
      command_line::add_arg(description, rmq_address);
      command_line::add_arg(description, rmq_credentials);
      command_line::add_arg(description, rmq_exchange);
      command_line::add_arg(description, rmq_routing);
#endif
      description.add_options()(rest_servers.name, boost::program_options::value<std::vector<std::string>>()->default_value({rest_default}, rest_default), rest_servers.description);
      command_line::add_arg(description, admin_rest_servers);
      command_line::add_arg(description, rest_ssl_key);
      command_line::add_arg(description, rest_ssl_cert);
      command_line::add_arg(description, rest_threads);
      command_line::add_arg(description, scan_threads);
      command_line::add_arg(description, access_controls);
      command_line::add_arg(description, external_bind);
      command_line::add_arg(description, create_queue_max);
      command_line::add_arg(description, rates_interval);
      command_line::add_arg(description, log_level);
      command_line::add_arg(description, disable_admin_auth);
      command_line::add_arg(description, webhook_ssl_verification);
      command_line::add_arg(description, config_file);
      command_line::add_arg(description, max_subaddresses);
      command_line::add_arg(description, auto_accept_creation);
      command_line::add_arg(description, untrusted_daemon);
    }
  };

  struct program
  {
    std::string db_path;
    std::vector<std::string> rest_servers;
    std::vector<std::string> admin_rest_servers;
    lws::rest_server::configuration rest_config;
    std::string daemon_rpc;
    std::string daemon_sub;
    std::string zmq_pub;
    lws::rpc::rmq_details rmq;
    std::string webhook_ssl_verification;
    std::chrono::minutes rates_interval;
    std::size_t scan_threads;
    unsigned create_queue_max;
    bool untrusted_daemon;
  };

  void print_help(std::ostream& out)
  {
    boost::program_options::options_description description{"Options"};
    options{}.prepare(description);

    out << "Usage: [options]" << std::endl;
    out << description;
  }

  boost::optional<program> get_program(int argc, char** argv)
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
      return boost::none;
    }

    opts.set_network(args); // do this first, sets global variable :/
    mlog_set_log_level(command_line::get_arg(args, opts.log_level));

    const auto webhook_verify_raw = command_line::get_arg(args, opts.webhook_ssl_verification);
    epee::net_utils::ssl_verification_t webhook_verify = epee::net_utils::ssl_verification_t::none;
    if (webhook_verify_raw == "system_ca")
      webhook_verify = epee::net_utils::ssl_verification_t::system_ca;
    else if (webhook_verify_raw != "none")
      MONERO_THROW(lws::error::configuration, "Invalid webhook ssl verification mode");

    program prog{
      command_line::get_arg(args, opts.db_path),
      command_line::get_arg(args, opts.rest_servers),
      command_line::get_arg(args, opts.admin_rest_servers),
      lws::rest_server::configuration{
        {command_line::get_arg(args, opts.rest_ssl_key), command_line::get_arg(args, opts.rest_ssl_cert)},
        command_line::get_arg(args, opts.access_controls),
        command_line::get_arg(args, opts.rest_threads),
	command_line::get_arg(args, opts.max_subaddresses),
        webhook_verify,
        command_line::get_arg(args, opts.external_bind),
        command_line::get_arg(args, opts.disable_admin_auth),
        command_line::get_arg(args, opts.auto_accept_creation)
      },
      command_line::get_arg(args, opts.daemon_rpc),
      command_line::get_arg(args, opts.daemon_sub),
      command_line::get_arg(args, opts.zmq_pub),
#ifdef MLWS_RMQ_ENABLED
      lws::rpc::rmq_details{
        command_line::get_arg(args, opts.rmq_address),
        command_line::get_arg(args, opts.rmq_credentials),
        command_line::get_arg(args, opts.rmq_exchange),
        command_line::get_arg(args, opts.rmq_routing)
      },
#else
      lws::rpc::rmq_details{},
#endif
      command_line::get_arg(args, opts.webhook_ssl_verification),
      std::chrono::minutes{command_line::get_arg(args, opts.rates_interval)},
      command_line::get_arg(args, opts.scan_threads),
      command_line::get_arg(args, opts.create_queue_max),
      command_line::get_arg(args, opts.untrusted_daemon)
    };

    prog.rest_config.threads = std::max(std::size_t(1), prog.rest_config.threads);
    prog.scan_threads = std::max(std::size_t(1), prog.scan_threads);

    if (command_line::is_arg_defaulted(args, opts.daemon_rpc))
      prog.daemon_rpc = options::get_default_zmq();

    return prog;
  }

  void run(program prog)
  {
    std::signal(SIGINT, [] (int) { lws::scanner::stop(); });

    boost::filesystem::create_directories(prog.db_path);
    auto disk = lws::db::storage::open(prog.db_path.c_str(), prog.create_queue_max);
    auto ctx = lws::rpc::context::make(std::move(prog.daemon_rpc), std::move(prog.daemon_sub), std::move(prog.zmq_pub), std::move(prog.rmq), prog.rates_interval, prog.untrusted_daemon);

    MINFO("Using monerod ZMQ RPC at " << ctx.daemon_address());
    auto client = lws::scanner::sync(disk.clone(), ctx.connect().value(), prog.untrusted_daemon).value();

    const auto enable_subaddresses = bool(prog.rest_config.max_subaddresses);
    const auto webhook_verify = prog.rest_config.webhook_verify;
    lws::rest_server server{
      epee::to_span(prog.rest_servers), prog.admin_rest_servers, disk.clone(), std::move(client), std::move(prog.rest_config)
    };
    for (const std::string& address : prog.rest_servers)
      MINFO("Listening for REST clients at " << address);
    for (const std::string& address : prog.admin_rest_servers)
      MINFO("Listening for REST admin clients at " << address);

    // blocks until SIGINT
    lws::scanner::run(std::move(disk), std::move(ctx), prog.scan_threads, webhook_verify, enable_subaddresses, prog.untrusted_daemon);
  }
} // anonymous

int main(int argc, char** argv)
{
  tools::on_startup(); // if it throws, don't use MERROR just print default msg

  try
  {
    boost::optional<program> prog;

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
    MERROR(e.what());
    return EXIT_FAILURE;
  }
  catch (...)
  {
    MERROR("Unknown exception");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
