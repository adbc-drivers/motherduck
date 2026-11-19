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

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libpq-fe.h>

#include "result_reader.h"

namespace adbcpq {
namespace {

constexpr Oid kInt4Oid = 23;
constexpr Oid kTextOid = 25;
constexpr Oid kFloat8Oid = 701;

struct Column {
  std::string name;
  Oid type;
};

// A result with the given columns and no rows, built without a server
void SetResult(PqResultHelper* helper, const std::vector<Column>& columns) {
  PGresult* result = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  ASSERT_NE(result, nullptr);
  std::vector<PGresAttDesc> attrs(columns.size());
  for (size_t i = 0; i < columns.size(); i++) {
    attrs[i] = PGresAttDesc{};
    attrs[i].name = const_cast<char*>(columns[i].name.c_str());
    attrs[i].format = 1;
    attrs[i].typid = columns[i].type;
    attrs[i].typlen = -1;
    attrs[i].atttypmod = -1;
  }
  ASSERT_EQ(PQsetResultAttrs(result, static_cast<int>(attrs.size()), attrs.data()), 1);
  helper->SetResult(result);
}

TEST(PostgresResultReaderTest, MatchingResult) {
  PqResultHelper helper(nullptr, "");
  SetResult(&helper, {{"a", kInt4Oid}, {"b", kTextOid}});
  EXPECT_TRUE(
      PqResultArrayReader::CheckResultMatches(helper, {kInt4Oid, kTextOid}, 3).ok());
}

TEST(PostgresResultReaderTest, ColumnCountMismatch) {
  PqResultHelper helper(nullptr, "");
  SetResult(&helper, {{"a", kInt4Oid}, {"b", kTextOid}, {"c", kTextOid}});
  Status status =
      PqResultArrayReader::CheckResultMatches(helper, {kInt4Oid, kTextOid}, 4);
  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status.message(), ::testing::HasSubstr("bind row 4 has 3 columns"));
  EXPECT_THAT(status.message(), ::testing::HasSubstr("first result had 2"));
}

TEST(PostgresResultReaderTest, ColumnTypeMismatch) {
  PqResultHelper helper(nullptr, "");
  SetResult(&helper, {{"a", kInt4Oid}, {"b", kFloat8Oid}});
  Status status =
      PqResultArrayReader::CheckResultMatches(helper, {kInt4Oid, kTextOid}, 7);
  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status.message(), ::testing::HasSubstr("bind row 7"));
  EXPECT_THAT(status.message(), ::testing::HasSubstr("type OID 701 for column 1,"));
  // The name could be SQL text, so it stays out of the message.
  EXPECT_THAT(status.message(), ::testing::Not(::testing::HasSubstr("\"b\"")));
  EXPECT_THAT(status.message(), ::testing::HasSubstr("first result had type OID 25"));
}

}  // namespace
}  // namespace adbcpq
