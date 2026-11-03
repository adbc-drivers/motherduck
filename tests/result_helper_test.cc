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
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "result_helper.h"

namespace adbcpq {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

namespace {

// libpq hands out NUL-terminated values, and an empty string for NULL.
PqRecord TextRecord(const char* value) {
  return PqRecord{value, static_cast<int>(std::strlen(value)), false};
}

std::vector<std::string> ParseOk(const char* value) {
  auto result = TextRecord(value).ParseTextArray();
  EXPECT_TRUE(result.has_value()) << "'" << value << "': " << result.status().message();
  return result.has_value() ? result.value() : std::vector<std::string>{};
}

}  // namespace

TEST(PqRecordTest, ParseTextArray) {
  EXPECT_THAT(ParseOk("{}"), IsEmpty());
  EXPECT_THAT(ParseOk("{ }"), IsEmpty());
  EXPECT_THAT(ParseOk("{a}"), ElementsAre("a"));
  EXPECT_THAT(ParseOk("{a,b}"), ElementsAre("a", "b"));
}

TEST(PqRecordTest, ParseTextArrayWhitespace) {
  EXPECT_THAT(ParseOk("{ a , b c ,d }"), ElementsAre("a", "b c", "d"));
  EXPECT_THAT(ParseOk("{ \" a \" , b}"), ElementsAre(" a ", "b"));
  // Escaped whitespace is part of the element and isn't trimmed.
  EXPECT_THAT(ParseOk("{a\\ }"), ElementsAre("a "));
}

TEST(PqRecordTest, ParseTextArrayQuoted) {
  EXPECT_THAT(ParseOk("{\"a,b\",c}"), ElementsAre("a,b", "c"));
  EXPECT_THAT(ParseOk("{\"\"}"), ElementsAre(""));
  EXPECT_THAT(ParseOk("{\"{x}\",\"NULL\"}"), ElementsAre("{x}", "NULL"));
  EXPECT_THAT(ParseOk("{\"has space\",plain}"), ElementsAre("has space", "plain"));
}

TEST(PqRecordTest, ParseTextArrayEscapes) {
  EXPECT_THAT(ParseOk("{\"a\\\"b\"}"), ElementsAre("a\"b"));
  EXPECT_THAT(ParseOk("{\"a\\\\b\"}"), ElementsAre("a\\b"));
  EXPECT_THAT(ParseOk("{\"\\\\\"}"), ElementsAre("\\"));
  EXPECT_THAT(ParseOk("{a\\,b,c}"), ElementsAre("a,b", "c"));
}

TEST(PqRecordTest, ParseTextArrayNullElement) {
  // Callers have no representation for a NULL element; it comes back as text.
  EXPECT_THAT(ParseOk("{a,NULL}"), ElementsAre("a", "NULL"));
}

TEST(PqRecordTest, ParseTextArrayMalformed) {
  for (const char* value :
       {"", "{", "}", "a,b", "{a,b", "a,b}", "{\"a}", "{\"a\\\"}", "{a\\}", "{a,}",
        "{,a}", "{a,,b}", "{\"a\"b}", "{a\"b\"}", "{{a}}", "{a}b}", "[1:1]={a}"}) {
    auto result = TextRecord(value).ParseTextArray();
    EXPECT_FALSE(result.has_value()) << "'" << value << "'";
    if (!result.has_value()) {
      EXPECT_EQ(result.status().ToAdbc(nullptr), ADBC_STATUS_IO) << "'" << value << "'";
    }
  }
  EXPECT_FALSE((PqRecord{"", 0, true}).ParseTextArray().has_value());
}

TEST(PqRecordTest, ParseDoubleIgnoresStaleErrno) {
  errno = ERANGE;
  EXPECT_EQ(TextRecord("1.5").ParseDouble(), 1.5);
  EXPECT_EQ(TextRecord("abc").ParseDouble(), std::nullopt);
}

TEST(PqRecordTest, ParseDoubleRejectsTrailingGarbage) {
  EXPECT_EQ(TextRecord("-2.5e3").ParseDouble(), -2500.0);
  EXPECT_EQ(TextRecord("1.5abc").ParseDouble(), std::nullopt);
  EXPECT_EQ(TextRecord("1.5 ").ParseDouble(), std::nullopt);
  EXPECT_EQ(TextRecord("").ParseDouble(), std::nullopt);
  EXPECT_EQ((PqRecord{"", 0, true}).ParseDouble(), std::nullopt);
}

TEST(PqRecordTest, ParseIntegerMalformedIsIO) {
  auto result = TextRecord("12x").ParseInteger();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.status().ToAdbc(nullptr), ADBC_STATUS_IO);
}

}  // namespace adbcpq
