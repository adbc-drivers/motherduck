// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "adbc_driver_motherduck.h"

#include <gtest/gtest.h>

TEST(MotherduckDriverTest, InitSupportedVersions) {
  for (const int version : {ADBC_VERSION_1_0_0, ADBC_VERSION_1_1_0}) {
    AdbcDriver driver = {};
    ASSERT_EQ(ADBC_STATUS_OK, AdbcDriverMotherduckInit(version, &driver, nullptr));
    ASSERT_NE(nullptr, driver.DatabaseNew);
    ASSERT_NE(nullptr, driver.ConnectionInit);
    ASSERT_NE(nullptr, driver.StatementExecuteQuery);
    if (driver.release) {
      ASSERT_EQ(ADBC_STATUS_OK, driver.release(&driver, nullptr));
    }
  }
}

// The generic entry point is not declared in adbc.h; driver managers look it up by name.
extern "C" AdbcStatusCode AdbcDriverInit(int version, void* raw_driver,
                                         struct AdbcError* error);

// AdbcDriverInit forwards to AdbcDriverMotherduckInit, which also installs the error
// message redactor. The redactor lives in the shared library, so it can't be observed
// from here; check that the two entry points hand back the same driver.
TEST(MotherduckDriverTest, GenericInitMatchesMotherduckInit) {
  AdbcDriver generic = {};
  AdbcDriver motherduck = {};
  ASSERT_EQ(ADBC_STATUS_OK, AdbcDriverInit(ADBC_VERSION_1_1_0, &generic, nullptr));
  ASSERT_EQ(ADBC_STATUS_OK,
            AdbcDriverMotherduckInit(ADBC_VERSION_1_1_0, &motherduck, nullptr));
  EXPECT_EQ(generic.DatabaseNew, motherduck.DatabaseNew);
  EXPECT_EQ(generic.ConnectionInit, motherduck.ConnectionInit);
  EXPECT_EQ(generic.StatementExecuteQuery, motherduck.StatementExecuteQuery);
  for (AdbcDriver* driver : {&generic, &motherduck}) {
    if (driver->release) {
      ASSERT_EQ(ADBC_STATUS_OK, driver->release(driver, nullptr));
    }
  }
}

TEST(MotherduckDriverTest, RejectUnsupportedVersion) {
  AdbcDriver driver = {};
  AdbcError error = {};
  EXPECT_EQ(ADBC_STATUS_NOT_IMPLEMENTED,
            AdbcDriverMotherduckInit(424242, &driver, &error));
  if (error.release) {
    error.release(&error);
  }
}

TEST(MotherduckDriverTest, DatabaseLifecycle) {
  AdbcDriver driver = {};
  ASSERT_EQ(ADBC_STATUS_OK,
            AdbcDriverMotherduckInit(ADBC_VERSION_1_1_0, &driver, nullptr));
  AdbcDatabase database = {};
  ASSERT_EQ(ADBC_STATUS_OK, driver.DatabaseNew(&database, nullptr));
  EXPECT_EQ(ADBC_STATUS_OK,
            driver.DatabaseSetOption(&database, "uri", "postgresql://localhost/postgres",
                                     nullptr));
  EXPECT_EQ(ADBC_STATUS_OK, driver.DatabaseRelease(&database, nullptr));
  EXPECT_EQ(nullptr, database.private_data);
  if (driver.release) {
    EXPECT_EQ(ADBC_STATUS_OK, driver.release(&driver, nullptr));
  }
}
