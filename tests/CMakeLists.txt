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

include(FetchContent)
include(GoogleTest)

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gmock)
  fetchcontent_declare(googletest
                       GIT_REPOSITORY https://github.com/google/googletest.git
                       GIT_TAG v1.15.2
                       GIT_SHALLOW TRUE)
  set(INSTALL_GTEST
      OFF
      CACHE BOOL "" FORCE)
  fetchcontent_makeavailable(googletest)
endif()

set(ADBC_UPSTREAM_C "${PROJECT_SOURCE_DIR}/third_party/arrow-adbc/c")

add_library(adbc_validation STATIC
            ${ADBC_UPSTREAM_C}/validation/adbc_validation.cc
            ${ADBC_UPSTREAM_C}/validation/adbc_validation_connection.cc
            ${ADBC_UPSTREAM_C}/validation/adbc_validation_database.cc
            ${ADBC_UPSTREAM_C}/validation/adbc_validation_statement.cc
            ${ADBC_UPSTREAM_C}/validation/adbc_validation_util.cc)
# The validation helpers call the ADBC API, which the driver library provides.
target_link_libraries(adbc_validation PUBLIC adbc_driver_common adbc_driver_motherduck
                                             GTest::gmock)

add_executable(motherduck_common_test ${ADBC_UPSTREAM_C}/driver/common/utils_test.cc)
target_link_libraries(motherduck_common_test PRIVATE adbc_driver_common GTest::gmock_main)
gtest_discover_tests(motherduck_common_test TEST_PREFIX common.)

add_executable(motherduck_framework_test
               ${ADBC_UPSTREAM_C}/driver/framework/base_driver_test.cc)
target_link_libraries(motherduck_framework_test PRIVATE adbc_driver_framework
                                                        GTest::gmock_main)
gtest_discover_tests(motherduck_framework_test TEST_PREFIX framework.)

add_executable(motherduck_driver_test driver_test.cc)
target_link_libraries(motherduck_driver_test PRIVATE adbc_driver_motherduck
                                                     GTest::gtest_main)

# Compile the complete inherited test suites even when no PostgreSQL server is
# configured. The default test run exercises their server-independent cases.
add_executable(motherduck_postgresql_test error_test.cc postgres_type_test.cc
                                          postgresql_test.cc result_helper_test.cc)
target_link_libraries(motherduck_postgresql_test
                      PRIVATE adbc_driver_motherduck adbc_driver_motherduck_impl
                              adbc_validation GTest::gmock_main)
target_compile_definitions(motherduck_postgresql_test
                           PRIVATE ADBC_POSTGRESQL_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
)
add_test(NAME postgresql_unit
         COMMAND motherduck_postgresql_test
                 --gtest_filter=PostgresErrorTest.*:PostgresTypeTest.*:MotherDuckAttachModeTest.*:MotherDuckSessionNameTest.*:MotherDuckPasswordTest.*:MotherDuckUsernameTest.*:MotherDuckRegionTest.*:SplitStatementsTest.*:IsCopyStatementTest.*:IsResultOkTest.*
)
# Keep the server-independent run from creating a MotherDuck test database.
set_tests_properties(postgresql_unit
                     PROPERTIES ENVIRONMENT_MODIFICATION
                                "MOTHERDUCK_TEST_SETUP_URI=unset:")

add_executable(motherduck_copy_test copy/postgres_copy_reader_test.cc
                                    copy/postgres_copy_writer_test.cc)
target_link_libraries(motherduck_copy_test
                      PRIVATE adbc_driver_motherduck adbc_driver_motherduck_impl
                              adbc_validation GTest::gmock_main)
add_test(NAME copy_unit COMMAND motherduck_copy_test)

if(MOTHERDUCK_TEST_POSTGRESQL)
  add_test(NAME postgresql_integration COMMAND motherduck_postgresql_test)
  set_tests_properties(postgresql_integration
                       PROPERTIES LABELS
                                  postgresql
                                  RUN_SERIAL
                                  TRUE
                                  TIMEOUT
                                  600)
endif()

if(WIN32)
  foreach(test_target motherduck_driver_test motherduck_postgresql_test
                      motherduck_copy_test)
    add_custom_command(TARGET ${test_target}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                               $<TARGET_FILE:adbc_driver_motherduck>
                               $<TARGET_FILE_DIR:${test_target}>)
  endforeach()
endif()

gtest_discover_tests(motherduck_driver_test TEST_PREFIX driver.)
