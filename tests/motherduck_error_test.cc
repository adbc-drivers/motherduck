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

#include <arrow-adbc/adbc.h>
#include <gtest/gtest.h>

#include "error.h"
#include "motherduck_error.h"

namespace adbcpq {

// The primary messages the endpoint sends, all with SQLSTATE XXUUU.
TEST(MotherduckErrorTest, MissingObjects) {
  EXPECT_TRUE(MotherduckIsMissingObject(
      "Invalid Input Error: relation \"\"nope\"\" does not exist"));
  EXPECT_TRUE(MotherduckIsMissingObject(
      "Invalid Input Error: relation \"\"nosuchschema\".\"t\"\" does not exist"));
  EXPECT_TRUE(
      MotherduckIsMissingObject("Binder Error: Catalog \"nosuchdb\" does not exist!"));
  EXPECT_TRUE(MotherduckIsMissingObject(
      "Catalog Error: Scalar Function with name nosuchfn does not exist!\n"
      "Did you mean \"nanosecond\"?"));
}

TEST(MotherduckErrorTest, OtherErrors) {
  EXPECT_FALSE(MotherduckIsMissingObject(nullptr));
  EXPECT_FALSE(MotherduckIsMissingObject("Invalid Input Error: bad input"));
  EXPECT_FALSE(MotherduckIsMissingObject(
      "Binder Error: Referenced column \"x\" not found in FROM clause!"));
  // The phrase alone, outside the server's error clause, doesn't count.
  EXPECT_FALSE(MotherduckIsMissingObject(
      "Parser Error: syntax error at or near \"relation x does not exist\""));
}

TEST(MotherduckErrorTest, ClassifyServerError) {
  EXPECT_EQ(ClassifyServerError("XXUUU", "Binder Error: Catalog \"x\" does not exist!"),
            ADBC_STATUS_NOT_FOUND);
  EXPECT_EQ(ClassifyServerError("XXUUU", "Invalid Input Error: bad input"),
            ADBC_STATUS_INTERNAL);
  EXPECT_EQ(ClassifyServerError("XXUUU", nullptr), ADBC_STATUS_INTERNAL);
  // Other SQLSTATEs classify as PostgreSQL's do, whatever the message.
  EXPECT_EQ(
      ClassifyServerError("42P01", "Catalog Error: Table with name t does not exist!"),
      ADBC_STATUS_NOT_FOUND);
  EXPECT_EQ(ClassifyServerError("22012", "Binder Error: Catalog \"x\" does not exist!"),
            ADBC_STATUS_INVALID_DATA);
  EXPECT_EQ(ClassifyServerError(nullptr, nullptr), ADBC_STATUS_IO);
}

}  // namespace adbcpq
