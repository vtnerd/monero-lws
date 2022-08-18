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

load_cache(${MONERO_BUILD_DIR} READ_WITH_PREFIX monero_
  Boost_THREAD_LIBRARY_RELEASE
  CMAKE_CXX_COMPILER
  EXTRA_LIBRARIES
  HIDAPI_INCLUDE_DIR
  LMDB_INCLUDE
  monero_SOURCE_DIR
  OPENSSL_CRYPTO_LIBRARY
  OPENSSL_SSL_LIBRARY
  SODIUM_LIBRARY
  UNBOUND_LIBRARIES
  ZMQ_INCLUDE_PATH
  ZMQ_LIB
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

# hidapi
# https://github.com/moneroexamples/openmonero/blob/18370af8fa51a0e9d05dfe678dcd5bcb7fad30f7/cmake/FindHIDAPI.cmake#L26-L60
find_library(HIDAPI_LIBRARY
  NAMES hidapi hidapi-libusb)

find_path(HIDAPI_INCLUDE_DIR
  NAMES hidapi.h
  PATH_SUFFIXES
  hidapi)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(HIDAPI
  DEFAULT_MSG
  HIDAPI_LIBRARY
  HIDAPI_INCLUDE_DIR)

if(HIDAPI_FOUND)
  set(HIDAPI_LIBRARIES "${HIDAPI_LIBRARY}")
  if((STATIC AND UNIX AND NOT APPLE) OR (DEPENDS AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    find_library(LIBUSB-1.0_LIBRARY usb-1.0)
    find_library(LIBUDEV_LIBRARY udev)
    if(LIBUSB-1.0_LIBRARY)
      set(HIDAPI_LIBRARIES "${HIDAPI_LIBRARIES};${LIBUSB-1.0_LIBRARY}")
      if(LIBUDEV_LIBRARY)
        set(HIDAPI_LIBRARIES "${HIDAPI_LIBRARIES};${LIBUDEV_LIBRARY}")
      else()
        message(WARNING "libudev library not found, binaries may fail to link.")
      endif()
    else()
      message(WARNING "libusb-1.0 library not found, binaries may fail to link.")
    endif()
  endif()

  set(HIDAPI_INCLUDE_DIRS "${HIDAPI_INCLUDE_DIR}")
endif()

mark_as_advanced(HIDAPI_INCLUDE_DIR HIDAPI_LIBRARY)

if (NOT (HIDAPI_INCLUDE_DIR STREQUAL monero_HIDAPI_INCLUDE_DIR))
  message(STATUS "Found HIDAPI_INCLUDE_DIR: ${HIDAPI_INCLUDE_DIR}")
  message(STATUS "Found monero_HIDAPI_INCLUDE_DIR: ${monero_HIDAPI_INCLUDE_DIR}")
  message(FATAL_ERROR "hidapi libraries for monero build differs from this project")
endif()

# openssl
if (STATIC)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

if (NOT (OPENSSL_CRYPTO_LIBRARY STREQUAL monero_OPENSSL_CRYPTO_LIBRARY))
  message(STATUS "Found OPENSSL_CRYPTO_LIBRARY: ${OPENSSL_CRYPTO_LIBRARY}")
  message(STATUS "Found monero_OPENSSL_CRYPTO_LIBRARY: ${monero_OPENSSL_CRYPTO_LIBRARY}")
  message(FATAL_ERROR "openssl libraries for monero build differs from this project")
endif()

# zmq
# https://github.com/monero-project/monero/blob/9aab19f349433687c7aaf2c1cbc5751e5912c0aa/CMakeLists.txt#L1171-L1199
if(STATIC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DZMQ_STATIC")
endif()
find_path(ZMQ_INCLUDE_PATH zmq.h)
find_library(ZMQ_LIB zmq)
find_library(PGM_LIBRARY pgm)
find_library(NORM_LIBRARY norm)
find_library(GSSAPI_LIBRARY gssapi_krb5)
find_library(PROTOLIB_LIBRARY protolib)
find_library(SODIUM_LIBRARY sodium)

if(NOT ZMQ_INCLUDE_PATH)
  message(FATAL_ERROR "Could not find required header zmq.h")
endif()
if(NOT ZMQ_LIB)
  message(FATAL_ERROR "Could not find required libzmq")
endif()
if(PGM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${PGM_LIBRARY}")
endif()
if(NORM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${NORM_LIBRARY}")
endif()
if(GSSAPI_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${GSSAPI_LIBRARY}")
endif()
if(PROTOLIB_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${PROTOLIB_LIBRARY}")
endif()
if(SODIUM_LIBRARY)
  set(ZMQ_LIB "${ZMQ_LIB};${SODIUM_LIBRARY}")
endif()

if (NOT (ZMQ_INCLUDE_PATH STREQUAL monero_ZMQ_INCLUDE_PATH))
  message(STATUS "Found ZMQ_INCLUDE_PATH: ${ZMQ_INCLUDE_PATH}")
  message(STATUS "Found monero_ZMQ_INCLUDE_PATH: ${monero_ZMQ_INCLUDE_PATH}")
  message(FATAL_ERROR "zmq libraries for monero build differs from this project")
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

if(APPLE)
  find_library(IOKIT_LIBRARY IOKit)
  list(APPEND IMPORTED_MONERO_LIBRARIES ${IOKIT_LIBRARY})
endif()

add_library(monero::libraries INTERFACE IMPORTED)
set_property(TARGET monero::libraries PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES
    ${Boost_INCLUDE_DIR}
    ${HIDAPI_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${ZMQ_INCLUDE_PATH}
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
    ${HIDAPI_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${monero_SODIUM_LIBRARY}
    ${monero_UNBOUND_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

set(LMDB_INCLUDE "${monero_LMDB_INCLUDE}")
set(LMDB_LIB_PATH "monero::lmdb")


#
# Build monero-lws code
#

add_subdirectory(src)
