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

#include <cstdint>
#include <string>

#include <arrow-adbc/adbc.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "motherduck_connection.h"

namespace adbcpq {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::StartsWith;

// The PostgreSQL server version must never be reported as MotherDuck's.
TEST(MotherduckInfoCodesTest, AllCodesOmitVendorVersion) {
  EXPECT_THAT(
      MotherduckInfoCodes(nullptr, 0),
      ElementsAre(ADBC_INFO_VENDOR_NAME, ADBC_INFO_DRIVER_NAME, ADBC_INFO_DRIVER_VERSION,
                  ADBC_INFO_DRIVER_ARROW_VERSION, ADBC_INFO_DRIVER_ADBC_VERSION));
}

TEST(MotherduckInfoCodesTest, RequestedCodesOmitVendorVersion) {
  const uint32_t codes[] = {ADBC_INFO_DRIVER_NAME, ADBC_INFO_VENDOR_VERSION,
                            ADBC_INFO_VENDOR_NAME};
  EXPECT_THAT(MotherduckInfoCodes(codes, 3),
              ElementsAre(ADBC_INFO_DRIVER_NAME, ADBC_INFO_VENDOR_NAME));

  const uint32_t only_version[] = {ADBC_INFO_VENDOR_VERSION};
  EXPECT_THAT(MotherduckInfoCodes(only_version, 1), IsEmpty());
}

class MotherduckStatisticsTest : public ::testing::Test {
 protected:
  void TearDown() override {
    if (error_.release) error_.release(&error_);
  }

  void ExpectNotSupported(AdbcStatusCode status, const char* function) {
    EXPECT_EQ(status, ADBC_STATUS_NOT_IMPLEMENTED);
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(std::string(error_.message),
                AllOf(StartsWith("[motherduck] "), HasSubstr(function),
                      HasSubstr("is not supported by the MotherDuck driver"),
                      HasSubstr("pg_stats")));
  }

  MotherduckConnection connection_;
  struct AdbcError error_ = ADBC_ERROR_INIT;
};

TEST_F(MotherduckStatisticsTest, GetStatistics) {
  struct ArrowArrayStream out = {};
  ExpectNotSupported(
      connection_.GetStatistics(nullptr, "main", nullptr, true, &out, &error_),
      "AdbcConnectionGetStatistics");
  EXPECT_EQ(out.release, nullptr);
}

TEST_F(MotherduckStatisticsTest, GetStatisticNames) {
  struct ArrowArrayStream out = {};
  ExpectNotSupported(connection_.GetStatisticNames(&out, &error_),
                     "AdbcConnectionGetStatisticNames");
  EXPECT_EQ(out.release, nullptr);
}

}  // namespace adbcpq
