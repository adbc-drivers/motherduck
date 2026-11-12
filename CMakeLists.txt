# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.24)

project(adbc_driver_motherduck
        VERSION 0.1.0
        DESCRIPTION "ADBC driver for MotherDuck using the PostgreSQL protocol"
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(GNUInstallDirs)
include(CTest)

option(MOTHERDUCK_TEST_POSTGRESQL "Run the inherited tests against PostgreSQL" OFF)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

configure_file(src/adbc_driver_motherduck_config.h.in
               generated/adbc_driver_motherduck_config.h @ONLY)

add_subdirectory(third_party/arrow-adbc)
add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS adbc_driver_motherduck
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/adbc_driver_motherduck.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY third_party/arrow-adbc/include/arrow-adbc
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE.txt NOTICE.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/adbc_driver_motherduck)
