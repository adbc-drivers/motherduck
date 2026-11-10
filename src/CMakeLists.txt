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
            motherduck/motherduck_statement.cc
            motherduck/motherduck_type_resolver.cc)
target_include_directories(adbc_driver_motherduck_impl
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/postgres
                                  ${CMAKE_CURRENT_SOURCE_DIR}/motherduck
                                  ${PROJECT_SOURCE_DIR}/include
                                  ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(adbc_driver_motherduck_impl PUBLIC adbc_driver_framework
                                                         PostgreSQL::PostgreSQL)
if(WIN32)
  target_link_libraries(adbc_driver_motherduck_impl PUBLIC wsock32 ws2_32)
endif()

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
