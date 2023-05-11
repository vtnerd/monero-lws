// Copyright (c) 2023, The Monero Project
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

#include "admin.h"

#include <boost/range/iterator_range.hpp>
#include <boost/uuid/random_generator.hpp>
#include <functional>
#include <utility>
#include "db/string.h"
#include "error.h"
#include "span.h" // monero/contrib/epee/include
#include "wire.h"
#include "wire/crypto.h"
#include "wire/error.h"
#include "wire/json/write.h"
#include "wire/traits.h"
#include "wire/uuid.h"
#include "wire/vector.h"

namespace wire
{
  static void write_bytes(wire::writer& dest, const std::pair<lws::db::webhook_key, std::vector<lws::db::webhook_value>>& self)
  {
    wire::object(dest, wire::field<0>("key", self.first), wire::field<1>("value", self.second));
  }
}

namespace
{
  // Do not output "full" debug data provided by `db::data.h` header; truncate output
  template<typename T>
  struct truncated
  {
    T value;
  };

  lws::db::account_address wire_unwrap(const boost::string_ref source)
  {
    const expect<lws::db::account_address> address = lws::db::address_string(source);
    if (!address)
      WIRE_DLOG_THROW(wire::error::schema::string, "Bad string to address conversion: " << address.error().message());
    return *address;
  }

  using base58_address = truncated<lws::db::account_address&>;
  void read_bytes(wire::reader& source, base58_address& dest)
  {
    dest.value = wire_unwrap(source.string());
  }

  void write_bytes(wire::writer& dest, const truncated<lws::db::account>& self)
  {
    wire::object(dest,
      wire::field("address", lws::db::address_string(self.value.address)),
      wire::field("scan_height", self.value.scan_height),
      wire::field("access_time", self.value.access)
    );
  }

  void write_bytes(wire::writer& dest, const truncated<lws::db::request_info>& self)
  {
    wire::object(dest,
      wire::field("address", lws::db::address_string(self.value.address)),
      wire::field("start_height", self.value.start_height)
    );
  }

  template<typename V>
  void write_bytes(wire::json_writer& dest, const truncated<boost::iterator_range<lmdb::value_iterator<V>>> self)
  {
    const auto truncate = [] (V src) { return truncated<V>{std::move(src)}; };
    wire::array(dest, std::move(self.value), truncate);
  }

  template<typename K, typename V, typename C>
  expect<void> stream_object(wire::json_writer& dest, expect<lmdb::key_stream<K, V, C>> self)
  {
    using value_range = boost::iterator_range<lmdb::value_iterator<V>>;
    const auto truncate = [] (value_range src) -> truncated<value_range>
    {
      return {std::move(src)};
    };

    if (!self)
      return self.error();

    wire::dynamic_object(dest, self->make_range(), wire::enum_as_string, truncate);
    return success();
  }

  template<typename T, typename... U>
  void read_addresses(wire::reader& source, T& self, U... field)
  {
    std::vector<std::string> addresses;
    wire::object(source, wire::field("addresses", std::ref(addresses)), std::move(field)...);

    self.addresses.reserve(addresses.size());
    for (const auto& elem : addresses)
      self.addresses.emplace_back(wire_unwrap(elem));
  }

  void write_addresses(wire::writer& dest, epee::span<const lws::db::account_address> self)
  {
    // writes an array of monero base58 address strings
    wire::object(dest, wire::field("updated", wire::as_array(self, lws::db::address_string)));
  }

  expect<void> write_addresses(wire::writer& dest, const expect<std::vector<lws::db::account_address>>& self)
  {
    if (!self)
      return self.error();
    write_addresses(dest, epee::to_span(*self));
    return success();
  }
} // anonymous

namespace lws { namespace rpc
{
  void read_bytes(wire::reader& source, add_account_req& self)
  {
    wire::object(source,
      wire::field("address", base58_address{self.address}),
      wire::field("key", std::ref(unwrap(unwrap(self.key))))
    );
  }

  void read_bytes(wire::reader& source, address_requests& self)
  {
    read_addresses(source, self, WIRE_FIELD(type));
  }
  void read_bytes(wire::reader& source, modify_account_req& self)
  {
    read_addresses(source, self, WIRE_FIELD(status));
  }
  void read_bytes(wire::reader& source, rescan_req& self)
  {
    read_addresses(source, self, WIRE_FIELD(height));
  }
  void read_bytes(wire::reader& source, webhook_add_req& self)
  {
    boost::optional<std::string> address;
    wire::object(source,
      WIRE_FIELD_ID(0, type),
      WIRE_FIELD_ID(1, url),
      WIRE_OPTIONAL_FIELD_ID(2, token),
      wire::optional_field<3>("address", std::ref(address)),
      WIRE_OPTIONAL_FIELD_ID(4, payment_id),
      WIRE_OPTIONAL_FIELD_ID(5, confirmations)
    );
    if (address)
      self.address = wire_unwrap(*address);
    else
      self.address.reset();
  }
  void read_bytes(wire::reader& source, webhook_delete_req& self)
  {
    read_addresses(source, self);
  }

  void read_bytes(wire::reader& source, webhook_delete_uuid_req& self)
  {
    wire::object(source, WIRE_FIELD_ID(0, event_ids));
  }

  expect<void> accept_requests_::operator()(wire::writer& dest, db::storage disk, const request& req) const
  {
    return write_addresses(dest, disk.accept_requests(req.type, epee::to_span(req.addresses)));
  }

  expect<void> add_account_::operator()(wire::writer& out, db::storage disk, const request& req) const
  {
    using span = epee::span<const lws::db::account_address>;
    MONERO_CHECK(disk.add_account(req.address, req.key));
    write_addresses(out, span{std::addressof(req.address), 1});
    return success();
  }

  expect<void> list_accounts_::operator()(wire::json_writer& dest, db::storage disk) const
  {
    auto reader = disk.start_read();
    if (!reader)
      return reader.error();
    return stream_object(dest, reader->get_accounts());
  }

  expect<void> list_requests_::operator()(wire::json_writer& dest, db::storage disk) const
  {
    auto reader = disk.start_read();
    if (!reader)
      return reader.error();
    return stream_object(dest, reader->get_requests());
  }

  expect<void> modify_account_::operator()(wire::writer& dest, db::storage disk, const request& req) const
  {
    return write_addresses(dest, disk.change_status(req.status, epee::to_span(req.addresses)));
  }

  expect<void> reject_requests_::operator()(wire::writer& dest, db::storage disk, const request& req) const
  {
    return write_addresses(dest, disk.reject_requests(req.type, epee::to_span(req.addresses)));
  }

  expect<void> rescan_::operator()(wire::writer& dest, db::storage disk, const request& req) const
  {
    return write_addresses(dest, disk.rescan(req.height, epee::to_span(req.addresses)));
  }

  expect<void> webhook_add_::operator()(wire::writer& dest, db::storage disk, request&& req) const
  {
    if (req.address)
    {
      std::uint64_t payment_id = 0;
      static_assert(sizeof(payment_id) == sizeof(crypto::hash8), "invalid memcpy");
      if (req.payment_id)
        std::memcpy(std::addressof(payment_id), std::addressof(*req.payment_id), sizeof(payment_id));
      db::webhook_value event{
        db::webhook_dupsort{payment_id, boost::uuids::random_generator{}()},
        db::webhook_data{
          std::move(req.url),
          std::move(req.token).value_or(std::string{}),
          req.confirmations.value_or(1)
        }
      };

      MONERO_CHECK(disk.add_webhook(req.type, *req.address, event));
      write_bytes(dest, event);
    }
    else if (req.type == db::webhook_type::tx_confirmation)
      return {error::bad_webhook};
    return success();
  }

  expect<void> webhook_delete_::operator()(wire::writer& dest, db::storage disk, const request& req) const
  {
    MONERO_CHECK(disk.clear_webhooks(epee::to_span(req.addresses)));
    wire::object(dest); // write empty object
    return success();
  }

  expect<void> webhook_del_uuid_::operator()(wire::writer& dest, db::storage disk, request req) const
  {
    MONERO_CHECK(disk.clear_webhooks(std::move(req.event_ids)));
    wire::object(dest); // write empty object
    return success();
  }
  
  expect<void> webhook_list_::operator()(wire::writer& dest, db::storage disk) const
  {
    std::vector<std::pair<db::webhook_key, std::vector<db::webhook_value>>> data;
    {
      auto reader = disk.start_read();
      if (!reader)
        return reader.error();
      auto data_ = reader->get_webhooks();
      if (!data_)
        return data_.error();
      data = std::move(*data_);
    }

    wire::object(dest, wire::field<0>("webhooks", data));
    return success();
  }
}} // lws // rpc
