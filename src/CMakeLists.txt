# Copyright (c) 2026 ADBC Drivers Contributors
#
# This file has been modified from its original version, which is
# under the Apache License:
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

add_library(adbc_driver_motherduck_impl STATIC
            postgres/connection.cc
            postgres/error.cc
            postgres/database.cc
            postgres/result_helper.cc
            postgres/result_reader.cc
            postgres/statement.cc
            postgres/type_resolver_init.cc
            motherduck/motherduck_connection.cc
            motherduck/motherduck_database.cc
            motherduck/motherduck_error.cc
            motherduck/motherduck_factory.cc
            motherduck/motherduck_get_objects.cc
            motherduck/motherduck_pipeline.cc
            motherduck/motherduck_redact.cc
            motherduck/motherduck_statement.cc
            motherduck/motherduck_trust_store.cc
            motherduck/motherduck_type_resolver.cc
            motherduck/auth/http_client.cc
            motherduck/auth/json.cc
            motherduck/auth/login.cc
            motherduck/auth/token_cache.cc
            ${PROJECT_SOURCE_DIR}/third_party/yyjson/yyjson.c)
target_include_directories(adbc_driver_motherduck_impl
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/postgres
                                  ${CMAKE_CURRENT_SOURCE_DIR}/motherduck
                                  ${PROJECT_SOURCE_DIR}/include
                                  ${PROJECT_BINARY_DIR}/generated)
target_include_directories(adbc_driver_motherduck_impl SYSTEM
                           PRIVATE ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib
                                   ${PROJECT_SOURCE_DIR}/third_party/yyjson)
# Leave out the yyjson features the driver doesn't use; this also rejects
# non-standard JSON.
target_compile_definitions(adbc_driver_motherduck_impl
                           PRIVATE YYJSON_DISABLE_INCR_READER=1
                                   YYJSON_DISABLE_FILE=1
                                   YYJSON_DISABLE_UTILS=1
                                   YYJSON_DISABLE_NON_STANDARD=1)
target_link_libraries(adbc_driver_motherduck_impl
                      PUBLIC adbc_driver_framework
                             PostgreSQL::PostgreSQL
                             OpenSSL::SSL
                             OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(adbc_driver_motherduck_impl PUBLIC wsock32 ws2_32 shell32 crypt32
                                                           advapi32)
elseif(APPLE)
  # The native trust store, for verifying servers and the login's requests
  target_link_libraries(adbc_driver_motherduck_impl PUBLIC "-framework CoreFoundation"
                                                           "-framework Security")
endif()
if(MSVC)
  # C4273 (inconsistent dll linkage) means a definition in this static library
  # was declared ADBC_EXPORT, which MSVC silently exports from the DLL. Test
  # binaries that also link this library then fail with LNK2005, so stop here.
  target_compile_options(adbc_driver_motherduck_impl PRIVATE /we4273)
endif()

# The whole driver as one static library, for tests that need its internals.
# Linking this instead of both the DLL and the impl library gives each test
# binary a single copy of the driver. ADBC_EXPORT is empty so adbc.h doesn't
# declare the Adbc* entry points dllimport for a static link.
add_library(adbc_driver_motherduck_static STATIC postgres/postgresql.cc
                                                 motherduck/motherduck.cc)
target_link_libraries(adbc_driver_motherduck_static PUBLIC adbc_driver_motherduck_impl)
target_include_directories(adbc_driver_motherduck_static
                           PUBLIC ${PROJECT_SOURCE_DIR}/include
                                  ${PROJECT_SOURCE_DIR}/third_party/arrow-adbc/include)
target_compile_definitions(adbc_driver_motherduck_static PUBLIC "ADBC_EXPORT=")

add_library(adbc_driver_motherduck SHARED postgres/postgresql.cc motherduck/motherduck.cc)
target_link_libraries(adbc_driver_motherduck PRIVATE adbc_driver_motherduck_impl)
target_include_directories(adbc_driver_motherduck
                           PUBLIC ${PROJECT_SOURCE_DIR}/include
                                  ${PROJECT_SOURCE_DIR}/third_party/arrow-adbc/include)
target_compile_definitions(adbc_driver_motherduck PRIVATE ADBC_EXPORTING)
if(NOT WIN32)
  target_compile_definitions(adbc_driver_motherduck
                             PRIVATE "ADBC_EXPORT=__attribute__((visibility(\"default\")))"
  )
endif()
set_target_properties(adbc_driver_motherduck PROPERTIES OUTPUT_NAME
                                                        adbc_driver_motherduck)
if(APPLE)
  target_link_options(adbc_driver_motherduck PRIVATE "LINKER:-exported_symbol,_Adbc*")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(adbc_driver_motherduck PRIVATE
                      "LINKER:--version-script,${PROJECT_SOURCE_DIR}/cmake/exports.map")
endif()
