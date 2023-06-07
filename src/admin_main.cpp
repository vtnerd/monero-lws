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

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <cassert>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/command_line.h" // monero/src
#include "common/expect.h"       // monero/src
#include "config.h"
#include "error.h"
#include "db/storage.h"
#include "db/string.h"
#include "options.h"
#include "misc_log_ex.h"  // monero/contrib/epee/include
#include "rpc/admin.h"
#include "span.h"         // monero/contrib/epee/include
#include "string_tools.h" // monero/contrib/epee/include
#include "wire/crypto.h"
#include "wire/filters.h"
#include "wire/json/write.h"
#include "wire/wrapper/array.h"
#include "wire/wrappers_impl.h"

namespace
{
  // wrapper for custom output for admin accounts
  template<typename T>
  struct admin_display
  {
    T value;
  };

  void write_bytes(wire::json_writer& dest, const admin_display<lws::db::account>& source)
  {
    wire::object(dest,
      wire::field("address", lws::db::address_string(source.value.address)),
      wire::field("key", std::cref(source.value.key))
    );
  }

  void write_bytes(wire::json_writer& dest, admin_display<boost::iterator_range<lmdb::value_iterator<lws::db::account>>> source)
  {
    const auto filter = [](const lws::db::account& src)
    { return bool(src.flags & lws::db::account_flags::admin_account); };
    const auto transform = [] (lws::db::account src)
    { return admin_display<lws::db::account>{std::move(src)}; };

    wire_write::bytes(dest, wire::array(source.value | boost::adaptors::filtered(filter) | boost::adaptors::transformed(transform)));
  }

  template<typename F, typename... T>
  void run_command(F f, std::ostream& dest, T&&... args)
  {
    wire::json_stream_writer stream{dest};
    MONERO_UNWRAP(f(stream, std::forward<T>(args)...));
    stream.finish();
  }

  struct options : lws::options
  {
    const command_line::arg_descriptor<bool> show_sensitive;
    const command_line::arg_descriptor<std::string> command;
    const command_line::arg_descriptor<std::vector<std::string>> arguments;

    options()
      : lws::options()
      , show_sensitive{"show-sensitive", "Show view keys", false}
      , command{"command", "Admin command to execute", ""}
      , arguments{"arguments", "Arguments to command"}
    {}

    void prepare(boost::program_options::options_description& description) const
    {
      lws::options::prepare(description);
      command_line::add_arg(description, show_sensitive);
      command_line::add_arg(description, command);
      command_line::add_arg(description, arguments);
    }
  };

  struct program
  {
    lws::db::storage disk;
    std::vector<std::string> arguments;
    bool show_sensitive;
  };

  crypto::secret_key get_key(std::string const& hex)
  {
    crypto::secret_key out{};
    if (!epee::string_tools::hex_to_pod(hex, out))
      MONERO_THROW(lws::error::bad_view_key, "View key has invalid hex");
    return out;
  }

  std::vector<lws::db::account_address> get_addresses(epee::span<const std::string> arguments)
  {
    // first entry is currently always some other option
    assert(!arguments.empty());
    arguments.remove_prefix(1);

    std::vector<lws::db::account_address> addresses{};
    addresses.reserve(arguments.size());
    for (std::string const& address : arguments)
      addresses.push_back(lws::db::address_string(address).value());
    return addresses;
  }

  void accept_requests(program prog, std::ostream& out)
  {
    if (prog.arguments.size() < 2)
      throw std::runtime_error{"accept_requests requires 2 or more arguments"};

    lws::rpc::address_requests req{
      get_addresses(epee::to_span(prog.arguments)),
      MONERO_UNWRAP(lws::db::request_from_string(prog.arguments[0]))
    };
    run_command(lws::rpc::accept_requests, out, std::move(prog.disk), std::move(req));
  }

  void add_account(program prog, std::ostream& out)
  {
    if (prog.arguments.size() != 2)
      throw std::runtime_error{"add_account needs exactly two arguments"};

    lws::rpc::add_account_req req{
      lws::db::address_string(prog.arguments[0]).value(),
      get_key(prog.arguments[1])
    };
    run_command(lws::rpc::add_account, out, std::move(prog.disk), std::move(req));
  }

  void create_admin(program prog, std::ostream& out)
  {
    if (!prog.arguments.empty())
      throw std::runtime_error{"create_admin takes zero arguments"};

    admin_display<lws::db::account> account{};
    {
      crypto::secret_key auth{};
      crypto::generate_keys(account.value.address.view_public, auth);
      MONERO_UNWRAP(prog.disk.add_account(account.value.address, auth, lws::db::account_flags::admin_account));

      static_assert(sizeof(auth) == sizeof(account.value.key), "bad memcpy");
      std::memcpy(std::addressof(account.value.key), std::addressof(auth), sizeof(auth));
    }

    wire::json_stream_writer json{out};
    write_bytes(json, account);
    json.finish();
  }

  void debug_database(program prog, std::ostream& out)
  {
    if (!prog.arguments.empty())
      throw std::runtime_error{"debug_database takes zero arguments"};

    auto reader = prog.disk.start_read().value();
    reader.json_debug(out, prog.show_sensitive);
  }

  void list_accounts(program prog, std::ostream& out)
  {
    if (!prog.arguments.empty())
      throw std::runtime_error{"list_accounts takes zero arguments"};
    run_command(lws::rpc::list_accounts, out, std::move(prog.disk));
  }

  void list_admin(program prog, std::ostream& out)
  {
    if (!prog.arguments.empty())
      throw std::runtime_error{"list_admin takes zero arguments"};

    using value_range = boost::iterator_range<lmdb::value_iterator<lws::db::account>>;
    const auto transform = [] (value_range user)
    { return admin_display<value_range>{std::move(user)}; };

    auto reader = MONERO_UNWRAP(prog.disk.start_read());
    wire::json_stream_writer json{out};
    wire::dynamic_object(
      json, reader.get_accounts().value().make_range(), wire::enum_as_string, transform
    );
    json.finish();
  }

  void list_requests(program prog, std::ostream& out)
  {
    if (!prog.arguments.empty())
      throw std::runtime_error{"list_requests takes zero arguments"};
    run_command(lws::rpc::list_requests, out, std::move(prog.disk));
  }

  void modify_account(program prog, std::ostream& out)
  {
    if (prog.arguments.size() < 2)
      throw std::runtime_error{"modify_account_status requires 2 or more arguments"};

    lws::rpc::modify_account_req req{
      get_addresses(epee::to_span(prog.arguments)),
      lws::db::account_status_from_string(prog.arguments[0]).value()
    };
    run_command(lws::rpc::modify_account, out, std::move(prog.disk), std::move(req));
  }

  void reject_requests(program prog, std::ostream& out)
  {
    if (prog.arguments.size() < 2)
      MONERO_THROW(common_error::kInvalidArgument, "reject_requests requires 2 or more arguments");

    lws::rpc::address_requests req{
      get_addresses(epee::to_span(prog.arguments)),
      lws::db::request_from_string(prog.arguments[0]).value()
    };
    run_command(lws::rpc::reject_requests, out, std::move(prog.disk), std::move(req));
  }

  void rescan(program prog, std::ostream& out)
  {
    if (prog.arguments.size() < 2)
      throw std::runtime_error{"rescan requires 2 or more arguments"};

    lws::rpc::rescan_req req{
      get_addresses(epee::to_span(prog.arguments)),
      lws::db::block_id(std::stoull(prog.arguments[0]))
    };
    run_command(lws::rpc::rescan, out, std::move(prog.disk), std::move(req));
  }

  void rollback(program prog, std::ostream& out)
  {
    if (prog.arguments.size() != 1)
      throw std::runtime_error{"rollback requires 1 argument"};

    const auto height = lws::db::block_id(std::stoull(prog.arguments[0]));
    MONERO_UNWRAP(prog.disk.rollback(height));

    wire::json_stream_writer json{out};
    wire::object(json, wire::field("new_height", height));
    json.finish();
  }

  struct command
  {
    char const* const name;
    void (*const handler)(program, std::ostream&);
    char const* const parameters;
  };

  static constexpr const command commands[] =
  {
    {"accept_requests",       &accept_requests, "<\"create\"|\"import\"> <base58 address> [base 58 address]..."},
    {"add_account",           &add_account,     "<base58 address> <view key hex>"},
    {"create_admin",          &create_admin,    ""},
    {"debug_database",        &debug_database,  ""},
    {"list_accounts",         &list_accounts,   ""},
    {"list_admin",            &list_admin,      ""},
    {"list_requests",         &list_requests,   ""},
    {"modify_account_status", &modify_account,  "<\"active\"|\"inactive\"|\"hidden\"> <base58 address> [base 58 address]..."},
    {"reject_requests",       &reject_requests, "<\"create\"|\"import\"> <base58 address> [base 58 address]..."},
    {"rescan",                &rescan,          "<height> <base58 address> [base 58 address]..."},
    {"rollback",              &rollback,        "<height>"}
  };

  void print_help(std::ostream& out)
  {
    boost::program_options::options_description description{"Options"};
    options{}.prepare(description);

    out << "Usage: [options] [command] [arguments]" << std::endl;
    out << description << std::endl;
    out << "Commands:" << std::endl;
    for (command cmd : commands)
    {
      out << "  " << cmd.name << "\t\t" << cmd.parameters << std::endl;
    }
  }

  boost::optional<std::pair<std::string, program>> get_program(int argc, char** argv)
  {
    namespace po = boost::program_options;

    const options opts{};
    po::variables_map args{};
    {
      po::options_description description{"Options"};
      opts.prepare(description);

      po::positional_options_description positional{};
      positional.add(opts.command.name, 1);
      positional.add(opts.arguments.name, -1);

      po::store(
        po::command_line_parser(argc, argv)
        .options(description).positional(positional).run()
        , args
      );
      po::notify(args);
    }

    if (command_line::get_arg(args, command_line::arg_help))
    {
      print_help(std::cout);
      return boost::none;
    }

    opts.set_network(args); // do this first, sets global variable :/

    program prog{
      lws::db::storage::open(command_line::get_arg(args, opts.db_path).c_str(), 0)
    };

    prog.show_sensitive = command_line::get_arg(args, opts.show_sensitive);
    auto cmd = args[opts.command.name];
    if (cmd.empty())
      throw std::runtime_error{"No command given"};

    prog.arguments = command_line::get_arg(args, opts.arguments);
    return {{cmd.as<std::string>(), std::move(prog)}};
  }

  void run(boost::string_ref name, program prog, std::ostream& out)
  {
    struct by_name
    {
      bool operator()(command const& left, command const& right) const noexcept
      {
        assert(left.name && right.name);
        return std::strcmp(left.name, right.name) < 0;
      }
      bool operator()(boost::string_ref left, command const& right) const noexcept
      {
        assert(right.name);
        return left < right.name;
      }
      bool operator()(command const& left, boost::string_ref right) const noexcept
      {
        assert(left.name);
        return left.name < right;
      }
    };

    assert(std::is_sorted(std::begin(commands), std::end(commands), by_name{}));
    const auto found = std::lower_bound(
      std::begin(commands), std::end(commands), name, by_name{}
    );
    if (found == std::end(commands) || found->name != name)
      throw std::runtime_error{"No such command"};

    assert(found->handler != nullptr);
    found->handler(std::move(prog), out);

    if (out.bad())
      MONERO_THROW(std::io_errc::stream, "Writing to stdout failed");

    out << std::endl;
  }
} // anonymous

int main (int argc, char** argv)
{
  try
  {
    mlog_configure("", false, 0, 0); // disable logging

    boost::optional<std::pair<std::string, program>> prog;

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
      run(prog->first, std::move(prog->second), std::cout);
  }
  catch (std::exception const& e)
  {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }
  catch (...)
  {
    std::cerr << "Unknown exception" << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
