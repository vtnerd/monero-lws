// Copyright (c) 2018, The Monero Project
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
#include "account.h"

#include <algorithm>
#include <cstring>

#include "carrot_core/account_secrets.h"
#include "common/error.h"
#include "common/expect.h"
#include "db/data.h"
#include "db/string.h"
#include "wire/adapted/crypto.h"
#include "wire/adapted/pair.h"
#include "wire/adapted/tuple.h"
#include "wire/msgpack.h"
#include "wire/vector.h"
#include "wire/wrapper/trusted_array.h"

namespace lws
{
  namespace
  {
    // update if `crypto::public_key` gets `operator<`
    struct sort_pubs
    {
      bool operator()(crypto::public_key const& lhs, crypto::public_key const& rhs) const noexcept
      {
        return std::memcmp(std::addressof(lhs), std::addressof(rhs), sizeof(lhs)) < 0;
      }
    };

    account::key_type get_type(const db::account_flags flags) noexcept
    {
      if (flags & db::account_flags::view_balance_key)
        return account::key_type::balance;
      return account::key_type::legacy;
    }
  }

  WIRE_AS_INTEGER(account::key_type);

  struct account::internal
  {
    internal()
      : address(),
        id(db::account_id::invalid),
        pubs{},
        view_key{},
        balance_key{},
        type(account::key_type::legacy)
    {}

    explicit internal(db::account const& source)
      : address(db::address_string(source.address)),
        id(source.id),
        pubs(source.address),
        view_key(),
        balance_key{},
        type(get_type(source.flags))
    {
      using inner_type =
        std::remove_reference<decltype(tools::unwrap(view_key))>::type;

      crypto::secret_key& target = type == account::key_type::balance ?
        balance_key : view_key; 

      static_assert(std::is_standard_layout<db::view_key>(), "need standard layout source");
      static_assert(std::is_pod<inner_type>(), "need pod target");
      static_assert(sizeof(target) == sizeof(source.key), "different size keys");
      std::memcpy(
        std::addressof(tools::unwrap(target)),
        std::addressof(source.key),
        sizeof(source.key)
      );

      if (type == account::key_type::balance)
        carrot::make_carrot_viewincoming_key(balance_key, view_key);
    }

    void read_bytes(wire::msgpack_reader& source)
    { map(source, *this); }

    void write_bytes(wire::msgpack_writer& dest) const
    { map(dest, *this); }

    template<typename F, typename T>
    static void map(F& format, T& self)
    {
      wire::object(format,
        WIRE_FIELD_ID(0, address),
        WIRE_FIELD_ID(1, id),
        WIRE_FIELD_ID(2, pubs),
        WIRE_FIELD_ID(3, view_key),
        WIRE_FIELD_ID(4, balance_key),
        WIRE_FIELD_ID(5, type)
      );
    }

    std::string address;
    db::account_id id;
    db::account_address pubs;
    crypto::secret_key view_key;
    crypto::secret_key balance_key;
    account::key_type type;
  };

  account::account(std::shared_ptr<const internal> immutable, db::block_id height, std::vector<spendable_t> spendable, std::vector<balance_t> balance, std::vector<crypto::public_key> pubs) noexcept
    : immutable_(std::move(immutable))
    , spendable_(std::move(spendable))
    , balance_(std::move(balance))
    , pubs_(std::move(pubs))
    , spends_()
    , outputs_()
    , height_(height)
  {}

  void account::null_check() const
  {
    if (!immutable_)
      MONERO_THROW(::common_error::kInvalidArgument, "using moved from account");
  }

  template<typename F, typename T, typename U>
  void account::map(F& format, T& self, U& immutable)
  {
    wire::object(format,
      wire::field<0>("immutable_", std::ref(immutable)),
      wire::optional_field<1>("spendable_", wire::trusted_array(std::ref(self.spendable_))),
      wire::optional_field<2>("pubs_", wire::trusted_array(std::ref(self.pubs_))),
      wire::optional_field<3>("spends_", wire::trusted_array(std::ref(self.spends_))),
      wire::optional_field<4>("outputs_", wire::trusted_array(std::ref(self.outputs_))),
      WIRE_FIELD_ID(5, height_),
      wire::optional_field<6>("balance_", wire::trusted_array(std::ref(self.balance_)))
    );
  }

  account::account() noexcept
    : immutable_(nullptr), spendable_(), balance_(), pubs_(), spends_(), outputs_(), height_(db::block_id(0))
  {}

  account::account(db::account const& source, std::vector<spendable_t> spendable, std::vector<balance_t> balance, std::vector<crypto::public_key> pubs)
    : account(std::make_shared<internal>(source), source.scan_height, std::move(spendable), std::move(balance), std::move(pubs))
  {
    std::sort(spendable_.begin(), spendable_.end());
    std::sort(balance_.begin(), balance_.end());
    std::sort(pubs_.begin(), pubs_.end(), sort_pubs{});
  }

  account::~account() noexcept
  {}

  void account::read_bytes(::wire::msgpack_reader& source)
  {
    auto immutable = std::make_shared<internal>();
    map(source, *this, *immutable);
    immutable_ = std::move(immutable);
    std::sort(spendable_.begin(), spendable_.end());
    std::sort(balance_.begin(), balance_.end());
    std::sort(pubs_.begin(), pubs_.end(), sort_pubs{});
  }

  void account::write_bytes(::wire::msgpack_writer& dest) const
  {
    null_check();
    map(dest, *this, *immutable_);
  }

  account account::clone() const
  {
    account result{immutable_, height_, spendable_, balance_, pubs_};
    result.outputs_ = outputs_;
    result.spends_ = spends_;
    return result;
  }

  void account::updated(db::block_id new_height) noexcept
  {
    height_ = std::max(height_, new_height);
    spends_.clear();
    spends_.shrink_to_fit();
    outputs_.clear();
    outputs_.shrink_to_fit();
  }

  db::account_id account::id() const noexcept
  {
    if (immutable_)
      return immutable_->id;
    return db::account_id::invalid;
  }

  account::key_type account::type() const noexcept
  {
    if (immutable_)
      return immutable_->type;
    return key_type::legacy;
  }

  std::string const& account::address() const
  {
    null_check();
    return immutable_->address;
  }

  db::account_address const& account::db_address() const
  {
    null_check();
    return immutable_->pubs;
  }

  crypto::public_key const& account::view_public() const
  {
    null_check();
    return immutable_->pubs.view_public;
  }

  crypto::public_key const& account::spend_public() const
  {
    null_check();
    return immutable_->pubs.spend_public;
  }

  crypto::secret_key const& account::view_key() const
  {
    null_check();
    return immutable_->view_key;
  }

  crypto::secret_key const& account::balance_key() const
  {
    null_check();
    return immutable_->balance_key;
  }

  crypto::secret_key account::image_key() const
  {
    crypto::secret_key imager{};
    if (immutable_)
      carrot::make_carrot_generateimage_key(immutable_->balance_key, imager);
    return imager;
  }

  std::optional<db::address_index> account::get_spendable(db::output_id const& id) const noexcept
  {
    const auto searchable = 
      std::make_pair(id, db::address_index::primary());
    const auto account =
      std::lower_bound(spendable_.begin(), spendable_.end(), searchable);
    if (account == spendable_.end() || account->first != id)
      return std::nullopt;
    return account->second;
  }

  std::optional<std::pair<db::output_id, db::address_index>> account::get_spendable(crypto::key_image const& image) const noexcept
  {
    const auto searchable =
      std::make_tuple(image, std::uint64_t(0), db::address_index::primary());
    const auto account =
      std::lower_bound(balance_.begin(), balance_.end(), searchable);
    if (account == balance_.end() || std::get<0>(*account) != image)
      return std::nullopt;
    return std::make_pair(db::output_id{0, std::get<1>(*account)}, std::get<2>(*account));
  }

  bool account::add_out(db::output const& out)
  {
    auto existing_pub = std::lower_bound(pubs_.begin(), pubs_.end(), out.pub, sort_pubs{});
    if (existing_pub != pubs_.end() && *existing_pub == out.pub)
      return false;

    pubs_.insert(existing_pub, out.pub);

    if (type() == key_type::balance)
    {
    }
    else
    {
      auto spendable_value = std::make_pair(out.spend_meta.id, out.recipient);
      spendable_.insert(
        std::lower_bound(spendable_.begin(), spendable_.end(), spendable_value),
        spendable_value
      );
    }
    outputs_.push_back(out);
    return true;
  }

  void account::add_spend(db::spend const& spend)
  {
    spends_.push_back(spend);
  }
} // lws


