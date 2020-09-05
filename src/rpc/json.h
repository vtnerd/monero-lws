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

#include "wire/json.h"

namespace lws
{
namespace rpc
{
  struct json_request_base
  {
    static constexpr const char jsonrpc[] = "2.0";

    //! `method` must be in static memory.
    explicit json_request_base(const char* method)
      : id(0), method(method)
    {}
    
    unsigned id;
    const char* method; //!< Must be in static memory
  };
  constexpr const char json_request_base::jsonrpc[];

  //! \tparam W implements the WRITE concept \tparam M implements the METHOD concept
  template<typename W, typename M>
  struct json_request : json_request_base
  {
    template<typename... U>
    explicit json_request(U&&... args)
      : json_request_base(M::name()),
        params{std::forward<U>(args)...}
    {}

    W params;
  };

  template<typename W, typename M>
  inline void write_bytes(wire::json_writer& dest, const json_request<W, M>& self)
  {
    // pull fields from base class into the same object
    wire::object(dest, WIRE_FIELD_COPY(id), WIRE_FIELD_COPY(jsonrpc), WIRE_FIELD_COPY(method), WIRE_FIELD(params));
  }


  //! \tparam R implements the READ concept
  template<typename R>
  struct json_response
  {
    json_response() = delete;

    unsigned id;
    R result;
  };

  template<typename R>
  inline void read_bytes(wire::json_reader& source, json_response<R>& self)
  {
    wire::object(source, WIRE_FIELD(id), WIRE_FIELD(result));
  }


  /*! Implements the RPC concept (JSON-RPC 2.0).
    \tparam M must implement the METHOD concept. */
  template<typename M>
  struct json
  {
    using wire_type = wire::json;
    using request = json_request<typename M::request, M>;
    using response = json_response<typename M::response>;
  };
} // rpc
} // lws
