// Copyright (c) 2026 ADBC Drivers Contributors
//
// This file has been modified from its original version, which is
// under the Apache License:
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <libpq-fe.h>

#include <gtest/gtest.h>
#include <libpq-fe.h>

#include "connection.h"
#include "error.h"
#include "postgres_type.h"
#include "statement.h"

namespace adbcpq {

TEST(PostgresErrorTest, RedactQueryTruncates) {
  EXPECT_EQ(RedactQuery("SELECT 1"), "SELECT 1");
  const std::string long_query = "SELECT '" + std::string(kMaxQueryBytes, 'a') + "'";
  const std::string out = RedactQuery(long_query);
  EXPECT_EQ(out, long_query.substr(0, kMaxQueryBytes) + "... [truncated]");
  // Server text has its own cap.
  const std::string long_message(kMaxMessageBytes + 1, 'a');
  EXPECT_EQ(RedactMessage(long_message),
            long_message.substr(0, kMaxMessageBytes) + "... [truncated]");
  EXPECT_EQ(RedactMessage("short"), "short");
}

TEST(PostgresErrorTest, RedactQueryKeepsUtf8Intact) {
  // U+00E9 is two bytes; put its lead byte last in the kept prefix.
  std::string query(kMaxQueryBytes - 1, 'a');
  query += "\xC3\xA9 tail";
  const std::string out = RedactQuery(query);
  EXPECT_EQ(out, std::string(kMaxQueryBytes - 1, 'a') + "... [truncated]");
}

TEST(PostgresErrorTest, RedactorSeesOnlyThePrefix) {
  SetQueryRedactor([](std::string_view text) { return std::to_string(text.size()); });
  const std::string long_query(kMaxQueryBytes + 500, 'x');
  EXPECT_EQ(RedactQuery(long_query), std::to_string(kMaxQueryBytes) + "... [truncated]");
  SetQueryRedactor(nullptr);
}

// Results libpq makes itself (lost connection) have a message but no fields. A failed
// connection gives one without a server.
TEST(PostgresErrorTest, PqErrorTextKeepsClientSideMessages) {
  PGconn* conn = PQconnectdb("host=127.0.0.1 port=1 connect_timeout=2");
  ASSERT_NE(conn, nullptr);
  ASSERT_EQ(PQstatus(conn), CONNECTION_BAD);
  const std::string conn_message = PQerrorMessage(conn);
  ASSERT_FALSE(conn_message.empty());

  PGresult* result = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY), nullptr);
  const std::string text = PqErrorText(result, conn);
  EXPECT_EQ(text, conn_message);
  EXPECT_EQ(text.find("missing error text"), std::string::npos) << text;
  PQclear(result);

  // Not an error result: use the connection's message.
  result = PQmakeEmptyPGresult(conn, PGRES_COMMAND_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PqErrorText(result, conn), conn_message);
  PQclear(result);

  // No result: the connection's message, or nothing.
  EXPECT_EQ(PqErrorText(nullptr, conn), conn_message);
  EXPECT_EQ(PqErrorText(nullptr, nullptr), "");
  PQfinish(conn);
}

// MakeStatus takes only numbers and RedactedText (a static_assert rejects the rest).
TEST(PostgresErrorTest, MakeStatusFormatsRedactedText) {
  const std::string message =
      MakeStatus(nullptr, "failed '{}' [{}] {}", RedactQuery("SELECT 1"),
                 PqStatusName(PGRES_FATAL_ERROR), 7)
          .message();
  EXPECT_EQ(message, "failed 'SELECT 1' [PGRES_FATAL_ERROR] 7");
}

TEST(PostgresErrorTest, ClassifySqlState) {
  struct Case {
    const char* sqlstate;
    AdbcStatusCode status;
  };

  const Case kCases[] = {
      {nullptr, ADBC_STATUS_IO},
      {"57014", ADBC_STATUS_CANCELLED},
      {"0A000", ADBC_STATUS_NOT_IMPLEMENTED},
      {"22012", ADBC_STATUS_INVALID_DATA},
      {"22P02", ADBC_STATUS_INVALID_DATA},
      {"23505", ADBC_STATUS_INTEGRITY},
      {"23503", ADBC_STATUS_INTEGRITY},
      {"28P01", ADBC_STATUS_UNAUTHENTICATED},
      {"28000", ADBC_STATUS_UNAUTHENTICATED},
      {"42501", ADBC_STATUS_UNAUTHORIZED},
      {"42P01", ADBC_STATUS_NOT_FOUND},
      {"42P02", ADBC_STATUS_NOT_FOUND},
      {"42703", ADBC_STATUS_NOT_FOUND},
      {"42602", ADBC_STATUS_NOT_FOUND},
      {"3D000", ADBC_STATUS_NOT_FOUND},
      {"58P01", ADBC_STATUS_NOT_FOUND},
      {"42701", ADBC_STATUS_ALREADY_EXISTS},
      {"42P03", ADBC_STATUS_ALREADY_EXISTS},
      {"42P07", ADBC_STATUS_ALREADY_EXISTS},
      {"42P05", ADBC_STATUS_ALREADY_EXISTS},
      {"42712", ADBC_STATUS_ALREADY_EXISTS},
      {"42710", ADBC_STATUS_ALREADY_EXISTS},
      {"58P02", ADBC_STATUS_ALREADY_EXISTS},
      {"25P02", ADBC_STATUS_INVALID_STATE},
      {"55P03", ADBC_STATUS_INVALID_STATE},
      {"XX000", ADBC_STATUS_INTERNAL},
      {"42601", ADBC_STATUS_INVALID_ARGUMENT},
      {"08006", ADBC_STATUS_IO},
      {"53000", ADBC_STATUS_IO},
      {"ZZ999", ADBC_STATUS_IO},
  };

  for (const auto& test_case : kCases) {
    EXPECT_EQ(test_case.status, ClassifySqlState(test_case.sqlstate))
        << (test_case.sqlstate ? test_case.sqlstate : "null");
  }
}

TEST(PostgresErrorTest, EscapeIdentifier) {
  // A connection that never reached a server still carries a client encoding,
  // which is all PQescapeIdentifier needs.
  PGconn* conn = PQconnectdb("host=/nonexistent-adbc-motherduck-test-socket-dir");
  ASSERT_NE(nullptr, conn);

  struct AdbcError error = ADBC_ERROR_INIT;
  std::string escaped;
  EXPECT_EQ(ADBC_STATUS_OK, EscapeIdentifier(conn, "my\"table", &escaped, &error));
  EXPECT_EQ("\"my\"\"table\"", escaped);
  if (error.release) error.release(&error);
  PQfinish(conn);
}

TEST(PostgresErrorTest, EscapeIdentifierFailure) {
  // PQescapeIdentifier fails without a connection, the same way it fails for
  // input that is invalid in the client encoding.
  struct AdbcError error = ADBC_ERROR_INIT;
  std::string escaped = "unchanged";
  EXPECT_EQ(ADBC_STATUS_INTERNAL,
            EscapeIdentifier(nullptr, "my_table", &escaped, &error));
  EXPECT_EQ("unchanged", escaped);
  ASSERT_NE(nullptr, error.message);
  EXPECT_NE(nullptr, std::strstr(error.message, "Could not escape identifier my_table"))
      << error.message;
  if (error.release) error.release(&error);
}

TEST(PostgresConnectionUnitTest, GetTableSchemaNullTableName) {
  PostgresConnection connection;
  struct ArrowSchema schema = {};
  struct AdbcError error = ADBC_ERROR_INIT;
  EXPECT_EQ(ADBC_STATUS_INVALID_ARGUMENT,
            connection.GetTableSchema(nullptr, nullptr, nullptr, &schema, &error));
  ASSERT_NE(nullptr, error.message);
  EXPECT_NE(nullptr, std::strstr(error.message, "table_name must not be NULL"))
      << error.message;
  EXPECT_EQ(nullptr, schema.release);
  if (error.release) error.release(&error);
}

TEST(PostgresConnectionUnitTest, SetCurrentSchemaNull) {
  PostgresConnection connection;
  struct AdbcError error = ADBC_ERROR_INIT;
  EXPECT_EQ(
      ADBC_STATUS_INVALID_ARGUMENT,
      connection.SetOption(ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, nullptr, &error));
  ASSERT_NE(nullptr, error.message);
  EXPECT_NE(nullptr, std::strstr(error.message, "requires a value")) << error.message;
  if (error.release) error.release(&error);
}

// The statement has no connection: the name checks must run before touching it.
static void ExpectCreateBulkTableRejects(const char* name0, const char* name1,
                                         const char* expected_message) {
  struct ArrowSchema child0 = {};
  child0.format = "i";
  child0.name = name0;
  struct ArrowSchema child1 = {};
  child1.format = "i";
  child1.name = name1;
  struct ArrowSchema* children[] = {&child0, &child1};
  struct ArrowSchema source_schema = {};
  source_schema.format = "+s";
  source_schema.n_children = 2;
  source_schema.children = children;

  PostgresStatement statement;
  std::string escaped_table;
  std::string escaped_field_list;
  PostgresType copy_target_types;
  bool has_copy_target_types = true;
  struct AdbcError error = ADBC_ERROR_INIT;
  EXPECT_EQ(ADBC_STATUS_INVALID_ARGUMENT,
            statement.CreateBulkTable("public", source_schema, &escaped_table,
                                      &escaped_field_list, &copy_target_types,
                                      &has_copy_target_types, &error));
  ASSERT_NE(nullptr, error.message);
  EXPECT_NE(nullptr, std::strstr(error.message, expected_message)) << error.message;
  EXPECT_TRUE(escaped_table.empty());
  EXPECT_TRUE(escaped_field_list.empty());
  if (error.release) error.release(&error);
}

TEST(PostgresStatementUnitTest, CreateBulkTableNullColumnName) {
  ExpectCreateBulkTableRejects("ok", nullptr, "column 1: column name must not be NULL");
}

TEST(PostgresStatementUnitTest, CreateBulkTableEmptyColumnName) {
  ExpectCreateBulkTableRejects("ok", "", "column 1: column name must not be empty");
}

TEST(PostgresStatementUnitTest, CreateBulkTableDuplicateColumnName) {
  ExpectCreateBulkTableRejects("id", "id", "column 1: duplicate column name id");
}

}  // namespace adbcpq
