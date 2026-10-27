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

set(FMT_INSTALL
    OFF
    CACHE BOOL "" FORCE)
set(FMT_TEST
    OFF
    CACHE BOOL "" FORCE)
set(FMT_DOC
    OFF
    CACHE BOOL "" FORCE)
add_subdirectory(c/vendor/fmt EXCLUDE_FROM_ALL)
add_subdirectory(c/vendor/nanoarrow EXCLUDE_FROM_ALL)

add_library(adbc_driver_common STATIC c/driver/common/utils.c)
target_include_directories(adbc_driver_common PUBLIC include c c/driver
                                                     c/vendor/portable-snippets/include)
target_link_libraries(adbc_driver_common PUBLIC nanoarrow::nanoarrow)

add_library(adbc_driver_framework STATIC c/driver/framework/objects.cc
                                         c/driver/framework/utility.cc)
target_link_libraries(adbc_driver_framework PUBLIC adbc_driver_common fmt::fmt)
