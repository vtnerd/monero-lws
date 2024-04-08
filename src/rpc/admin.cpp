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

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/uuid/random_generator.hpp>
#include <functional>
#include <utility>
#include "db/string.h"
#include "error.h"
#include "span.h" // monero/contrib/epee/include
#include "wire.h"
#include "wire/adapted/crypto.h"
#include "wire/error.h"
#include "wire/json/write.h"
#include "wire/traits.h"
#include "wire/uuid.h"
#include "wire/vector.h"
#include "wire/wrapper/array.h"
#include "wire/wrappers_impl.h"

namespace wire
{
  static void write_bytes(wire::writer& dest, const std::pair<lws::db::webhook_key, std::vector<lws::db::webhook_value>>& self)
  {
    wire::object(dest, wire::field<0>("key", std::cref(self.first)), wire::field<1>("value", std::cref(self.second)));
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
    wire_write::bytes(dest, wire::array(boost::adaptors::transform(std::move(self.value), truncate)));
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
    using min_address_size =
      wire::min_element_sizeof<crypto::public_key, crypto::public_key>;

    std::vector<std::string> addresses;
    wire::object(source,
      wire::field("addresses", wire::array<min_address_size>(std::ref(addresses))),
      std::move(field)...
    );

    self.addresses.reserve(addresses.size());
    for (const auto& elem : addresses)
      self.addresses.emplace_back(wire_unwrap(elem));
  }

  void write_addresses(wire::writer& dest, epee::span<const lws::db::account_address> self)
  {
    // writes an array of monero base58 address strings

    wire::object(dest,
      wire::field("updated", wire::array(boost::adaptors::transform(self, lws::db::address_string)))
    );
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
  void read_bytes(wire::reader& source, validate_req& self)
  {
    wire::object(source, WIRE_FIELD(spend_public_hex), WIRE_FIELD(view_public_hex), WIRE_FIELD(view_key_hex));
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

  namespace
  {
    struct validate_error
    {
      std::string field;
      std::string details;
    };

    void write_bytes(wire::writer& dest, const validate_error& self)
    {
      wire::object(dest, WIRE_FIELD(field), WIRE_FIELD(details));
    }

    expect<void> output_error(wire::writer& dest, std::string field, std::string details)
    {
      wire::object(dest, wire::field("error", validate_error{std::move(field), std::move(details)}));
      return success();
    }

    template<typename T>
    bool convert_key(wire::writer& dest, T& out, const boost::string_ref in, const boost::string_ref field)
    {
      if (in.size() != sizeof(out) * 2)
      {
        output_error(dest, std::string{field}, "Expected " + std::to_string(sizeof(out) * 2) + " characters");
        return false;
      }
      if (!epee::from_hex::to_buffer(epee::as_mut_byte_span(out), in))
      {
        output_error(dest, std::string{field}, "Invalid hex");
        return false;
      }
      return true;
    }
  }

  expect<void> validate_::operator()(wire::writer& dest, const db::storage&, const request& req) const
  {
    db::account_address address{};
    crypto::secret_key view_key{};

    if (!convert_key(dest, address.spend_public, req.spend_public_hex, "spend_public_hex"))
      return success(); // error is delivered in JSON as opposed to HTTP codes
    if (!convert_key(dest, address.view_public, req.view_public_hex, "view_public_hex"))
      return success();
    if (!convert_key(dest, unwrap(unwrap(view_key)), req.view_key_hex, "view_key_hex"))
      return success();

    if (!crypto::check_key(address.spend_public))
      return output_error(dest, "spend_public_hex", "Invalid public key format");
    if (!crypto::check_key(address.view_public))
      return output_error(dest, "view_public_hex", "Invalid public key format");

    crypto::public_key test{};
    if (!crypto::secret_key_to_public_key(view_key, test) || test != address.view_public)
      return output_error(dest, "view_key_hex", "view_key and view_public do not match");

    wire::object(dest, wire::field("address", db::address_string(address)));
    return success();
  }

  expect<void> webhook_add_::operator()(wire::writer& dest, db::storage disk, request&& req) const
  {
    switch (req.type)
    {
      case db::webhook_type::tx_confirmation:
        if (!req.address)
          return {error::bad_webhook};
        break;
      case db::webhook_type::new_account:
        if (req.address)
          return {error::bad_webhook};
        break;
      case db::webhook_type::tx_spend:
        if (!req.address)
          return {error::bad_webhook};
        break;
      default:
        return {error::bad_webhook};
    }

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

    MONERO_CHECK(disk.add_webhook(req.type, req.address, event));
    write_bytes(dest, event);
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
