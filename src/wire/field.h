// Copyright (c) 2020, The Monero Project
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

#include <functional>
#include <utility>

#include "wire/filters.h"
#include "wire/traits.h"

//! A required field has the same key name and C/C++ name
#define WIRE_FIELD(name)                                \
  ::wire::field( #name , std::ref( self . name ))

//! A required field has the same key name and C/C++ name AND is cheap to copy (faster output).
#define WIRE_FIELD_COPY(name)                   \
  ::wire::field( #name , self . name )

//! The optional field has the same key name and C/C++ name
#define WIRE_OPTIONAL_FIELD(name)                               \
  ::wire::optional_field( #name , std::ref( self . name ))

namespace wire
{
  template<typename T>
  struct unwrap_reference
  {
    using type = T;
  };

  template<typename T>
  struct unwrap_reference<std::reference_wrapper<T>>
  {
    using type = T;
  };


  //! Links `name` to a `value` for object serialization.
  template<typename T, bool Required>
  struct field_
  {
    using value_type = typename unwrap_reference<T>::type;
    static constexpr bool is_required() noexcept { return Required; }
    static constexpr std::size_t count() noexcept { return 1; }

    const char* name;
    T value;

    //! \return `value` with `std::reference_wrapper` removed.
    constexpr const value_type& get_value() const noexcept
    {
      return value;
    }

    //! \return `value` with `std::reference_wrapper` removed.
    value_type& get_value() noexcept
    {
      return value;
    }
  };

  //! Links `name` to `value`. Use `std::ref` if de-serializing.
  template<typename T>
  constexpr inline field_<T, true> field(const char* name, T value)
  {
    return {name, std::move(value)};
  }

  //! Links `name` to `value`. Use `std::ref` if de-serializing.
  template<typename T>
  constexpr inline field_<T, false> optional_field(const char* name, T value)
  {
    return {name, std::move(value)};
  }


  //! Links `name` to a type `T` for variant serialization.
  template<typename T>
  struct option
  {
    const char* name;
  };

  //! \return Name associated with type `T` for variant `field`.
  template<typename T, typename U>
  constexpr const char* get_option_name(const U& field) noexcept
  {
    return static_cast< const option<T>& >(field).name;
  }

  //! Links each type in a variant to a string key.
  template<typename T, bool Required, typename... U>
  struct variant_field_ : option<U>...
  {
    using value_type = typename unwrap_reference<T>::type;
    static constexpr bool is_required() noexcept { return Required; }
    static constexpr std::size_t count() noexcept { return sizeof...(U); }

    constexpr variant_field_(const char* name, T value, option<U>... opts)
      : option<U>(std::move(opts))..., name(name), value(std::move(value))
    {}

    const char* name;
    T value;

    constexpr const value_type& get_value() const noexcept
    {
      return value;
    }

    value_type& get_value() noexcept
    {
      return value;
    }

    template<typename V>
    struct wrap
    {
      using result_type = void;

      variant_field_ self;
      V visitor;

      template<typename X>
      void operator()(const X& value) const
      {
        visitor(get_option_name<X>(self), value);
      }
    };

    template<typename V>
    void visit(V visitor) const
    {
      apply_visitor(wrap<V>{*this, std::move(visitor)}, get_value());
    }
  };

  //! Links variant `value` to a unique name per type in `opts`. Use `std::ref` for `value` if de-serializing.
  template<typename T, typename... U>
  constexpr inline variant_field_<T, true, U...> variant_field(const char* name, T value, option<U>... opts)
  {
    return {name, std::move(value), std::move(opts)...};
  }


  //! Indicates a field value should be written as an array
  template<typename T, typename F>
  struct as_array_
  {
    using value_type = typename unwrap_reference<T>::type;

    T value;
    F filter; //!< Each element in `value` given to this callable before `write_bytes`.

    //! \return `value` with `std::reference_wrapper` removed.
    constexpr const value_type& get_value() const noexcept
    {
      return value;
    }

    //! \return `value` with `std::reference_wrapper` removed.
    value_type& get_value() noexcept
    {
      return value;
    }
  };

  //! Callable that can filter `as_object` values or be used immediately.
  template<typename Default>
  struct as_array_filter
  {
    Default default_filter;

    template<typename T>
    constexpr as_array_<T, Default> operator()(T value) const
    {
      return {std::move(value), default_filter};
    }

    template<typename T, typename F>
    constexpr as_array_<T, F> operator()(T value, F filter) const
    {
      return {std::move(value), std::move(filter)};
    }
  };
  //! Usage: `wire::field("foo", wire::as_array(self.foo, to_string{})`. Consider `std::ref`.
  constexpr as_array_filter<identity_> as_array{};


  //! Indicates a field value should be written as an object
  template<typename T, typename F, typename G>
  struct as_object_
  {
    using map_type = typename unwrap_reference<T>::type;

    T map;
    F key_filter;   //!< Each key (`.first`) in `map` given to this callable before writing field key.
    G value_filter; //!< Each value (`.second`) in `map` given to this callable before `write_bytes`.

    //! \return `map` with `std::reference_wrapper` removed.
    constexpr const map_type& get_map() const noexcept
    {
      return map;
    }

    //! \return `map` with `std::reference_wrapper` removed.
    map_type& get_map() noexcept
    {
      return map;
    }
  };

  //! Usage: `wire::field("foo", wire::as_object(self.foo, to_string{}, wire::as_array))`. Consider `std::ref`.
  template<typename T, typename F = identity_, typename G = identity_>
  inline constexpr as_object_<T, F, G> as_object(T map, F key_filter = F{}, G value_filter = G{})
  {
    return {std::move(map), std::move(key_filter), std::move(value_filter)};
  }


  template<typename T>
  inline constexpr bool available(const field_<T, true>&) noexcept
  {
    return true;
  }
  template<typename T>
  inline bool available(const field_<T, false>& elem)
  {
    return bool(elem.get_value());
  }
  template<typename T, typename... U>
  inline constexpr bool available(const variant_field_<T, true, U...>&) noexcept
  {
    return true;
  }
  template<typename T, typename... U>
  inline constexpr bool available(const variant_field_<T, false, U...>& elem)
  {
    return elem != nullptr;
  }


  // example usage : `wire::sum(std::size_t(wire::available(fields))...)`

  inline constexpr int sum() noexcept
  {
    return 0;
  }
  template<typename T, typename... U>
  inline constexpr T sum(const T head, const U... tail) noexcept
  {
    return head + sum(tail...);
  }
}

