# Copyright (c) 2020, The Monero Project
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are
# permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list of
#    conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this list
#    of conditions and the following disclaimer in the documentation and/or other
#    materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors may be
#    used to endorse or promote products derived from this software without specific
#    prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

cmake_minimum_required(VERSION 3.1.0)
project(monero-lws)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MONERO_LIBRARIES
  daemon_messages
  serialization
  lmdb_lib
  net
  cryptonote_core
  cryptonote_basic
  cryptonote_format_utils_basic
  ringct
  ringct_basic
  multisig
  hardforks
  checkpoints
  blockchain_db
  common
  lmdb
  device
  cncrypto
  randomx
  epee
  easylogging
  version
  wallet-crypto
  unbound
)

set(MONERO_OPTIONAL unbound wallet-crypto)

set(MONERO_SEARCH_PATHS
  "/contrib/epee/src"
  "/external/db_drivers/liblmdb"
  "/external/easylogging++"
  "/src"
  "/src/crypto"
  "/src/crypto/wallet"
  "/src/cryptonote_basic"
  "/src/lmdb"
  "/src/ringct"
  "/src/rpc"
)


#
# Pull some information from monero build
#

# Needed due to "bug" in monero CMake - the `project` function is used twice!
if (NOT MONERO_SOURCE_DIR)
  message(FATAL_ERROR "The argument -DMONERO_SOURCE_DIR must specify a location of a monero source tree")
endif()

if (NOT MONERO_BUILD_DIR)
  message(FATAL_ERROR "The argument -DMONERO_BUILD_DIR must specify a location of an existing monero build")
endif()

if(STATIC)
  if(MSVC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .dll.a .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  endif()
endif()

load_cache(${MONERO_BUILD_DIR} READ_WITH_PREFIX monero_
  Boost_THREAD_LIBRARY_RELEASE
  CMAKE_CXX_COMPILER
  EXTRA_LIBRARIES
  LIBUDEV_LIBRARY
  usb_LIBRARY
  HIDAPI_INCLUDE_DIR
  HIDAPI_LIBRARY
  LMDB_INCLUDE
  monero_SOURCE_DIR
  OPENSSL_INCLUDE_PATH
  OPENSSL_CRYPTO_LIBRARY
  OPENSSL_SSL_LIBRARY
  SODIUM_LIBRARY
  UNBOUND_LIBRARIES
  ZMQ_INCLUDE_PATH
  ZMQ_LIB
  PGM_LIBRARY
  NORM_LIBRARY
  GSSAPI_LIBRARY
  PROTOLIB_LIBRARY
  SODIUM_LIBRARY
)

if (NOT (monero_monero_SOURCE_DIR MATCHES "${MONERO_SOURCE_DIR}(/src/cryptonote_protocol)"))
  message(FATAL_ERROR "Invalid Monero source dir - does not appear to match source used for build directory")
endif()

if (NOT (CMAKE_CXX_COMPILER STREQUAL monero_CMAKE_CXX_COMPILER))
  message(FATAL_ERROR "Compiler for monero build differs from this project")
endif()

if ("${monero_UNBOUND_LIBRARIES}" STREQUAL "UNBOUND_LIBRARIES-NOTFOUND")
  unset(monero_UNBOUND_LIBRARIES)
endif()

#
# Dependencies specific to monero-lws
#

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# boost
set(Boost_NO_BOOST_CMAKE ON)
if(STATIC)
  set(Boost_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_RUNTIME ON)
endif()
find_package(Boost 1.58 QUIET REQUIRED COMPONENTS chrono filesystem program_options regex serialization system thread)

if (NOT (Boost_THREAD_LIBRARY STREQUAL monero_Boost_THREAD_LIBRARY_RELEASE))
  message(STATUS "Found Boost_THREAD_LIBRARY: ${Boost_THREAD_LIBRARY}")
  message(STATUS "Found monero_Boost_THREAD_LIBRARY_RELEASE: ${monero_Boost_THREAD_LIBRARY_RELEASE}")
  message(FATAL_ERROR "Boost libraries for monero build differs from this project")
endif()


set(LMDB_INCLUDE "${monero_LMDB_INCLUDE}")
set(LMDB_LIB_PATH "monero::lmdb")
set(ZMQ_LIB "${monero_ZMQ_LIB}")

if(NOT ZMQ_LIB)
  message(FATAL_ERROR "Could not find required libzmq")
endif()
if(monero_PGM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_PGM_LIBRARY}")
endif()
if(monero_NORM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_NORM_LIBRARY}")
endif()
if(monero_GSSAPI_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_GSSAPI_LIBRARY}")
endif()
if(monero_PROTOLIB_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_PROTOLIB_LIBRARY}")
endif()
if(monero_SODIUM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_SODIUM_LIBRARY}")
endif()

if(STATIC AND NOT IOS)
  if(UNIX)
    set(monero_OPENSSL_LIBRARIES "${monero_OPENSSL_LIBRARIES};${CMAKE_DL_LIBS};${CMAKE_THREAD_LIBS_INIT}")
  endif()
endif()

foreach (LIB ${MONERO_LIBRARIES})
  find_library(LIB_PATH NAMES "${LIB}" PATHS ${MONERO_BUILD_DIR} PATH_SUFFIXES "/src/${LIB}" "external/${LIB}" ${MONERO_SEARCH_PATHS} NO_DEFAULT_PATH)

  list(FIND MONERO_OPTIONAL "${LIB}" LIB_OPTIONAL)
  if (NOT LIB_PATH)
    if (LIB_OPTIONAL EQUAL -1)
      message(FATAL_ERROR "Unable to find required Monero library ${LIB}")
    endif()
  else ()
    set(LIB_NAME "monero::${LIB}")
    add_library(${LIB_NAME} STATIC IMPORTED)
    set_target_properties(${LIB_NAME} PROPERTIES IMPORTED_LOCATION ${LIB_PATH})
    list(APPEND IMPORTED_MONERO_LIBRARIES "${LIB_NAME}")
  endif()
  unset(LIB_PATH CACHE)
endforeach()

add_library(monero::libraries INTERFACE IMPORTED)
set_property(TARGET monero::libraries PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES
    ${Boost_INCLUDE_DIR}
    ${monero_HIDAPI_INCLUDE_DIRS}
    ${monero_OPENSSL_INCLUDE_PATH}
    "${MONERO_BUILD_DIR}/generated_include"
    "${MONERO_SOURCE_DIR}/contrib/epee/include"
    "${MONERO_SOURCE_DIR}/external/easylogging++"
    "${MONERO_SOURCE_DIR}/external/rapidjson/include"
    "${MONERO_SOURCE_DIR}/external/supercop/include"
    "${MONERO_SOURCE_DIR}/src"
)
set_property(TARGET monero::libraries PROPERTY
  INTERFACE_LINK_LIBRARIES
    ${IMPORTED_MONERO_LIBRARIES}
    ${Boost_CHRONO_LIBRARY}
    ${Boost_FILESYSTEM_LIBRARY}
    ${Boost_REGEX_LIBRARY}
    ${Boost_SERIALIZATION_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_THREAD_LIBRARY}
    ${monero_HIDAPI_LIBRARY}
    ${monero_usb_LIBRARY}
    ${monero_LIBUDEV_LIBRARY}
    ${ZMQ_LIB}
    ${monero_OPENSSL_SSL_LIBRARY}
    ${monero_OPENSSL_CRYPTO_LIBRARY}
    ${monero_SODIUM_LIBRARY}
    ${monero_UNBOUND_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

#
# Build monero-lws code
#

add_subdirectory(src)
