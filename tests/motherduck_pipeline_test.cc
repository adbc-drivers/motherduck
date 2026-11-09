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

#include "motherduck_pipeline.h"

namespace adbcpq {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

TEST(SplitStatementsTest, Single) {
  EXPECT_THAT(SplitStatements("SELECT 1"), ElementsAre("SELECT 1"));
  EXPECT_THAT(SplitStatements("  SELECT 1;  "), ElementsAre("SELECT 1"));
  EXPECT_THAT(SplitStatements("SELECT 1; -- trailing"), ElementsAre("SELECT 1"));
}

TEST(SplitStatementsTest, Empty) {
  EXPECT_THAT(SplitStatements(""), IsEmpty());
  EXPECT_THAT(SplitStatements(" ;; \n; "), IsEmpty());
  EXPECT_THAT(SplitStatements("-- only a comment"), IsEmpty());
  EXPECT_THAT(SplitStatements("/* only */ ; /* comments */"), IsEmpty());
}

TEST(SplitStatementsTest, Multiple) {
  EXPECT_THAT(SplitStatements("CREATE TABLE t (i INT);\n  INSERT INTO t VALUES (1);\n"),
              ElementsAre("CREATE TABLE t (i INT)", "INSERT INTO t VALUES (1)"));
  EXPECT_THAT(SplitStatements("SELECT 1;;SELECT 2"), ElementsAre("SELECT 1", "SELECT 2"));
}

TEST(SplitStatementsTest, Quotes) {
  EXPECT_THAT(SplitStatements("SELECT ';'; SELECT 'it''s; here'"),
              ElementsAre("SELECT ';'", "SELECT 'it''s; here'"));
  EXPECT_THAT(SplitStatements(R"(SELECT "a;b" FROM "t""; x"; SELECT 2)"),
              ElementsAre(R"(SELECT "a;b" FROM "t""; x")", "SELECT 2"));
  // Backslashes escape quotes only in E'' strings.
  EXPECT_THAT(SplitStatements(R"(SELECT E'\'; '; SELECT 'a\'; SELECT 3)"),
              ElementsAre(R"(SELECT E'\'; ')", R"(SELECT 'a\')", "SELECT 3"));
  EXPECT_THAT(SplitStatements(R"(SELECT note'\'; SELECT 2)"),
              ElementsAre(R"(SELECT note'\')", "SELECT 2"));
  // With standard_conforming_strings off, they escape quotes in every literal.
  EXPECT_THAT(SplitStatements(R"(SELECT 'a\'; b'; SELECT 2)", /*backslash_escapes=*/true),
              ElementsAre(R"(SELECT 'a\'; b')", "SELECT 2"));
}

TEST(SplitStatementsTest, DollarQuotes) {
  EXPECT_THAT(SplitStatements("SELECT $$a;b$$; SELECT $fn$ $$; $fn$; SELECT 3"),
              ElementsAre("SELECT $$a;b$$", "SELECT $fn$ $$; $fn$", "SELECT 3"));
  // Parameters and identifiers containing $ don't open a quote.
  EXPECT_THAT(SplitStatements("SELECT $1; SELECT a$b$ FROM t; SELECT $2"),
              ElementsAre("SELECT $1", "SELECT a$b$ FROM t", "SELECT $2"));
}

TEST(SplitStatementsTest, Comments) {
  EXPECT_THAT(SplitStatements("SELECT 1 -- a; b\n; SELECT 2"),
              ElementsAre("SELECT 1 -- a; b", "SELECT 2"));
  EXPECT_THAT(SplitStatements("SELECT /* a; /* nested; */ b; */ 1; SELECT 2"),
              ElementsAre("SELECT /* a; /* nested; */ b; */ 1", "SELECT 2"));
  EXPECT_THAT(SplitStatements("-- drop it\nDROP TABLE t; SELECT 1"),
              ElementsAre("-- drop it\nDROP TABLE t", "SELECT 1"));
}

TEST(SplitStatementsTest, Unterminated) {
  EXPECT_THAT(SplitStatements("SELECT 1; SELECT 'a; b"),
              ElementsAre("SELECT 1", "SELECT 'a; b"));
  EXPECT_THAT(SplitStatements("SELECT 1; SELECT /* a; b"),
              ElementsAre("SELECT 1", "SELECT /* a; b"));
}

TEST(CastParametersTest, Casts) {
  EXPECT_EQ(CastParameters("SELECT $1", {"int4"}), "SELECT CAST($1 AS int4)");
  EXPECT_EQ(CastParameters("SELECT 1 + $2, $1, $2", {"text", "int8"}),
            "SELECT 1 + CAST($2 AS int8), CAST($1 AS text), CAST($2 AS int8)");
  EXPECT_EQ(CastParameters("SELECT $1::int4", {"text ARRAY"}),
            "SELECT CAST($1 AS text ARRAY)::int4");
  EXPECT_EQ(CastParameters("SELECT $10", std::vector<std::string>(10, "bool")),
            "SELECT CAST($10 AS bool)");
}

TEST(CastParametersTest, LeavesUntyped) {
  // No type, or a parameter past the bound ones.
  EXPECT_EQ(CastParameters("SELECT $1, $2, $3", {"", "int4"}),
            "SELECT $1, CAST($2 AS int4), $3");
  EXPECT_EQ(CastParameters("SELECT $0, $99999999999999999999999", {"int4"}),
            "SELECT $0, $99999999999999999999999");
  // 2^64 + 3, which wraps to 3 if the parse overflows.
  EXPECT_EQ(CastParameters("SELECT $18446744073709551619", {"int4", "int4", "int4"}),
            "SELECT $18446744073709551619");
  EXPECT_EQ(CastParameters("SELECT 1", {"int4"}), "SELECT 1");
}

TEST(CastParametersTest, SkipsNonParameters) {
  EXPECT_EQ(CastParameters(R"(SELECT '$1', "$1", E'\'$1', $1)", {"int4"}),
            R"(SELECT '$1', "$1", E'\'$1', CAST($1 AS int4))");
  EXPECT_EQ(CastParameters("SELECT $$ $1 $$, $q$ $1 $q$, a$1, $1", {"int4"}),
            "SELECT $$ $1 $$, $q$ $1 $q$, a$1, CAST($1 AS int4)");
  EXPECT_EQ(CastParameters("SELECT $1 -- $1\n/* $1 /* $1 */ */", {"int4"}),
            "SELECT CAST($1 AS int4) -- $1\n/* $1 /* $1 */ */");
  EXPECT_EQ(CastParameters(R"(SELECT 'a\', $1)", {"int4"}),
            R"(SELECT 'a\', CAST($1 AS int4))");
  EXPECT_EQ(CastParameters(R"(SELECT 'a\', $1)", {"int4"}, /*backslash_escapes=*/true),
            R"(SELECT 'a\', $1)");
}

TEST(UncastParametersTest, Uncasts) {
  const std::vector<std::string> types = {"int4", "", "numeric(10, 5)"};
  EXPECT_EQ(UncastParameters("CAST($1 AS INTEGER)", types), "$1");
  EXPECT_EQ(UncastParameters("(CAST($1 AS INTEGER) + 1)", types), "($1 + 1)");
  EXPECT_EQ(UncastParameters("CAST($3 AS DECIMAL(10,5))", types), "$3");
  EXPECT_EQ(
      UncastParameters("f(CAST($1 AS INTEGER[]), CAST($3 AS DECIMAL(10,5)))", types),
      "f($1, $3)");
}

TEST(UncastParametersTest, LeavesOthers) {
  const std::vector<std::string> types = {"int4", ""};
  // Parameters that weren't cast, or past the bound ones
  EXPECT_EQ(UncastParameters("CAST($2 AS INTEGER)", types), "CAST($2 AS INTEGER)");
  EXPECT_EQ(UncastParameters("CAST($3 AS INTEGER)", types), "CAST($3 AS INTEGER)");
  EXPECT_EQ(UncastParameters("CAST(x AS INTEGER)", types), "CAST(x AS INTEGER)");
  EXPECT_EQ(UncastParameters("CAST($1 AS INTEGER", types), "CAST($1 AS INTEGER");
  EXPECT_EQ(UncastParameters("v", types), "v");
}

TEST(IsCopyStatementTest, Copy) {
  EXPECT_TRUE(IsCopyStatement("COPY t FROM STDIN"));
  EXPECT_TRUE(IsCopyStatement("copy (SELECT 1) TO STDOUT"));
  EXPECT_TRUE(
      IsCopyStatement("  -- load it\n/* a /* nested */ b */ Copy t FROM 'x.csv'"));
  EXPECT_TRUE(IsCopyStatement("COPY"));
}

TEST(IsCopyStatementTest, NotCopy) {
  EXPECT_FALSE(IsCopyStatement(""));
  EXPECT_FALSE(IsCopyStatement("SELECT 'COPY'"));
  EXPECT_FALSE(IsCopyStatement("COPYRIGHT"));
  EXPECT_FALSE(IsCopyStatement("cop"));
  EXPECT_FALSE(IsCopyStatement("-- COPY t FROM STDIN\nSELECT 1"));
}

TEST(IsResultOkTest, Statuses) {
  for (ExecStatusType status : {PGRES_COMMAND_OK, PGRES_TUPLES_OK, PGRES_EMPTY_QUERY,
                                PGRES_FATAL_ERROR, PGRES_COPY_IN}) {
    PGresult* result = PQmakeEmptyPGresult(nullptr, status);
    ASSERT_NE(result, nullptr);
    // MotherDuck answers DDL with PGRES_TUPLES_OK.
    EXPECT_EQ(IsResultOk(result), status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
        << PQresStatus(status);
    PQclear(result);
  }
}

}  // namespace adbcpq
