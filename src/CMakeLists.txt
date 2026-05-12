# Copyright (c) 2018-2020, The Monero Project
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

include_directories(.)

find_package(Git)

set(MLWS_COMMIT_BRANCH "")
set(MLWS_COMMIT_DATE "")
set(MLWS_COMMIT_HASH "")
if (GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE MLWS_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE MLWS_COMMIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" log -1 "--date=format:\"%Y/%m/%d %T\"" "--format=\"%ad\""
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE MLWS_COMMIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif ()

configure_file(lws_version.h.in "${CMAKE_BINARY_DIR}/generated_include/lws_version.h")
include_directories("${CMAKE_BINARY_DIR}/generated_include")

add_subdirectory(lmdb)
add_subdirectory(wire)
add_subdirectory(db)
add_subdirectory(net)
add_subdirectory(rpc)
add_subdirectory(util)

# For both the server and admin utility.
set(monero-lws-common_sources config.cpp error.cpp)
set(monero-lws-common_headers config.h error.h fwd.h)

add_library(monero-lws-common ${monero-lws-common_sources} ${monero-lws-common_headers})
target_link_libraries(monero-lws-common monero::libraries)

add_library(monero-lws-daemon-common rest_server.cpp scanner.cpp mempool.cpp)
target_include_directories(monero-lws-daemon-common PUBLIC ${ZMQ_INCLUDE_PATH})
target_link_libraries(monero-lws-daemon-common
  PUBLIC
    monero::libraries
    ${MONERO_lmdb}
    monero-lws-common
    monero-lws-db
    monero-lws-net
    monero-lws-rpc
    monero-lws-rpc-scanner
    monero-lws-wire-json
    monero-lws-util
    ${Boost_CHRONO_LIBRARY}
    ${Boost_COROUTINE_LIBRARY}
    ${Boost_CONTEXT_LIBRARY}
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_THREAD_LIBRARY}
    ${Boost_THREAD_LIBS_INIT}
    ${EXTRA_LIBRARIES}
    ${ZMQ_LIB}
    ${SODIUM_LIBRARY}
    Threads::Threads
)

add_executable(monero-lws-daemon server_main.cpp)
target_link_libraries(monero-lws-daemon
  PRIVATE
    monero::libraries
    monero-lws-daemon-common
    monero-lws-rpc-scanner
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    ${Boost_FILESYSTEM_LIBRARY}
)

add_executable(monero-lws-admin admin_main.cpp)
target_link_libraries(monero-lws-admin
  PRIVATE
    monero::libraries
    monero-lws-common
    monero-lws-db
    monero-lws-rpc
    monero-lws-wire-json
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    Threads::Threads
)

add_executable(monero-lws-client client_main.cpp)
target_link_libraries(monero-lws-client
  PRIVATE
    monero::libraries
    monero-lws-common
    monero-lws-daemon-common
    monero-lws-rpc
    monero-lws-rpc-scanner
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    Threads::Threads
)

install(TARGETS monero-lws-daemon DESTINATION bin)
install(TARGETS monero-lws-admin DESTINATION bin)
install(TARGETS monero-lws-client DESTINATION bin)
