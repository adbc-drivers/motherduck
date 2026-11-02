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

#include <cerrno>
#include <cstring>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "result_helper.h"

namespace adbcpq {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// libpq hands out NUL-terminated values, and an empty string for NULL.
PqRecord TextRecord(const char* value) {
  return PqRecord{value, static_cast<int>(std::strlen(value)), false};
}

TEST(PqRecordTest, ParseTextArray) {
  auto empty = TextRecord("{}").ParseTextArray();
  ASSERT_TRUE(empty.has_value()) << empty.status().message();
  EXPECT_THAT(empty.value(), IsEmpty());

  auto two = TextRecord("{a,b}").ParseTextArray();
  ASSERT_TRUE(two.has_value()) << two.status().message();
  EXPECT_THAT(two.value(), ElementsAre("a", "b"));
}

TEST(PqRecordTest, ParseTextArrayMalformed) {
  for (const char* value : {"", "{", "}", "a,b", "{a,b", "a,b}"}) {
    EXPECT_FALSE(TextRecord(value).ParseTextArray().has_value()) << "'" << value << "'";
  }
  EXPECT_FALSE((PqRecord{"", 0, true}).ParseTextArray().has_value());
}

TEST(PqRecordTest, ParseDoubleIgnoresStaleErrno) {
  errno = ERANGE;
  EXPECT_EQ(TextRecord("1.5").ParseDouble(), 1.5);
  EXPECT_EQ(TextRecord("abc").ParseDouble(), std::nullopt);
}

}  // namespace adbcpq
