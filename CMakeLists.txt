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

cmake_minimum_required(VERSION 3.5.0)
project(monero-lws)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DBOOST_UUID_DISABLE_ALIGNMENT) # This restores UUID's std::has_unique_object_representations property

option(BUILD_TESTS "Build Tests" OFF)
option(WITH_RMQ "Build with RMQ publish support" OFF)
if (WITH_RMQ)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DMLWS_RMQ_ENABLED")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DMLWS_RMQ_ENABLED")
endif()

if(STATIC)
  if(MSVC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .dll.a .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  endif()
endif()

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
)

set(MONERO_OPTIONAL wallet-crypto)

if (MONERO_BUILD_DIR)

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

  # monero `master` and `release-v0.18` branches use different LIBSODIUM
  # find routines. So the upstream cmake names differ

  load_cache(${MONERO_BUILD_DIR} READ_WITH_PREFIX monero_
    Boost_THREAD_LIBRARY_RELEASE
    CMAKE_CXX_COMPILER
    EXTRA_LIBRARIES
    LIBUDEV_LIBRARY
    usb_LIBRARY
    HIDAPI_INCLUDE_DIR
    HIDAPI_LIBRARY
    libzmq_INCLUDE_DIRS
    LMDB_INCLUDE
    monero_SOURCE_DIR
    OPENSSL_INCLUDE_DIR
    OPENSSL_CRYPTO_LIBRARY
    OPENSSL_SSL_LIBRARY
    pkgcfg_lib_libzmq_zmq
    sodium_LIBRARY_RELEASE
    SODIUM_LIBRARY
    UNBOUND_LIBRARIES
    PGM_LIBRARY
    NORM_LIBRARY
    GSSAPI_LIBRARY
    PROTOLIB_LIBRARY
  )

  if (NOT (monero_monero_SOURCE_DIR MATCHES "${MONERO_SOURCE_DIR}"))
    message(FATAL_ERROR "Invalid Monero source dir - does not appear to match source used for build directory")
  endif()

  if (NOT (CMAKE_CXX_COMPILER STREQUAL monero_CMAKE_CXX_COMPILER))
    message(FATAL_ERROR "Compiler for monero build differs from this project")
  endif()

  if ("${monero_UNBOUND_LIBRARIES}" STREQUAL "UNBOUND_LIBRARIES-NOTFOUND")
    unset(monero_UNBOUND_LIBRARIES)
  endif()

  if ("${monero_HIDAPI_LIBRARY}" STREQUAL "HIDAPI_LIBRARY-NOTFOUND")
    unset(monero_HIDAPI_INCLUDE_DIR)
    unset(monero_HIDAPI_LIBRARY)
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

  set(LMDB_INCLUDE "${monero_LMDB_INCLUDE}")
  set(LMDB_LIB_PATH "monero::lmdb")
else () # NOT MONERO_BUILD_DIR
  if (NOT MONERO_SOURCE_DIR)
    set (MONERO_SOURCE_DIR "${monero-lws_SOURCE_DIR}/external/monero")
  endif ()

  include(FetchContent)
  FetchContent_Declare(monero SOURCE_DIR "${MONERO_SOURCE_DIR}")

  if (NOT monero_POPULATED)
    FetchContent_MakeAvailable(monero)
  endif ()

  set(MONERO_BUILD_DIR "${monero_BINARY_DIR}")
  set(IMPORTED_MONERO_LIBRARIES "${MONERO_LIBRARIES}")
  set(monero_Boost_THREAD_LIBRARY_RELEASE "${Boost_THREAD_LIBRARY_RELEASE}")
  set(monero_pkgcfg_lib_libzmq_zmq "${pkgcfg_lib_libzmq_zmq}")
  if (SODIUM_LIBRARY)
    set(monero_SODIUM_LIBRARY "${SODIUM_LIBRARY}")
  else()
    set(monero_SODIUM_LIBRARY "${monero_sodium_LIBRARY_RELEASE}")
  endif()
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
find_package(Boost 1.70 QUIET REQUIRED COMPONENTS chrono context coroutine filesystem program_options regex serialization system thread)

if (NOT (Boost_THREAD_LIBRARY STREQUAL monero_Boost_THREAD_LIBRARY_RELEASE))
  message(STATUS "Found Boost_THREAD_LIBRARY: ${Boost_THREAD_LIBRARY}")
  message(STATUS "Found monero_Boost_THREAD_LIBRARY_RELEASE: ${monero_Boost_THREAD_LIBRARY_RELEASE}")
  message(FATAL_ERROR "Boost libraries for monero build differs from this project")
endif()

if (WITH_RMQ)
  find_path(RMQ_INCLUDE_DIR "amqp.h")
  find_library(RMQ_LIBRARY rabbitmq REQUIRED)
else()
  set(RMQ_INCLUDE_DIR "")
  set(RMQ_LIBRARY "")
endif()

set(ZMQ_INCLUDE_PATH "${libzmq_INCLUDE_DIRS}")
set(ZMQ_LIB "${monero_pkgcfg_lib_libzmq_zmq}")
if (monero_SODIUM_LIBRARY)
  set(SODIUM_LIBRARY "${monero_SODIUM_LIBRARY}")
else ()
  set(SODIUM_LIBRARY "${monero_sodium_LIBRARY_RELEASE}")
endif ()

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
if(monero_sodium_LIBRARY_RELEASE)
  set(ZMQ_LIB "${ZMQ_LIB};${monero_sodium_LIBRARY_RELEASE}")
endif()

if(STATIC AND NOT IOS)
  if(UNIX)
    set(monero_OPENSSL_LIBRARIES "${monero_OPENSSL_LIBRARIES};${CMAKE_DL_LIBS};${CMAKE_THREAD_LIBS_INIT}")
  endif()
endif()

if(APPLE)
  find_library(IOKIT_LIBRARY IOKit)
  list(APPEND IMPORTED_MONERO_LIBRARIES ${IOKIT_LIBRARY})
endif()

add_library(monero::libraries INTERFACE IMPORTED)
set_property(TARGET monero::libraries PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES
    ${Boost_INCLUDE_DIR}
    ${monero_HIDAPI_INCLUDE_DIRS}
    ${monero_OPENSSL_INCLUDE_DIR}
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
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    ${Boost_REGEX_LIBRARY}
    ${Boost_SERIALIZATION_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_THREAD_LIBRARY}
    ${monero_HIDAPI_LIBRARY}
    ${monero_usb_LIBRARY}
    ${monero_LIBUDEV_LIBRARY}
    ${monero_OPENSSL_SSL_LIBRARY}
    ${monero_OPENSSL_CRYPTO_LIBRARY}
    ${SODIUM_LIBRARY}
    ${monero_UNBOUND_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

#
# Build monero-lws code
#

add_subdirectory(src)

if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
