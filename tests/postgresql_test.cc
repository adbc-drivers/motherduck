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

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/driver/postgresql.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow.hpp>

#include "adbc_driver_motherduck_config.h"
#include "common/options.h"
#include "common/utils.h"
#include "motherduck_database.h"
#include "motherduck_test_util.h"
#include "validation/adbc_validation.h"
#include "validation/adbc_validation_util.h"

using adbc_validation::Handle;
using adbc_validation::IsOkStatus;
using adbc_validation::IsStatus;
using std::string_literals::operator""s;

// Bulk ingest needs COPY FROM STDIN, which the Postgres endpoint lacks (#31).
#define SKIP_WITHOUT_COPY_FROM_STDIN() GTEST_SKIP() << "Needs COPY FROM STDIN (#31)"

namespace {

// This process's own schema, adbc_test_<Unix time>_<process ID>_<run>, where
// <run> is MOTHERDUCK_TEST_RUN (see ci/scripts/pre-test.sh). The time is when
// the process started, which later runs go by to drop what an interrupted run
// left behind (IsStaleTestName), and the process ID keeps runs that source
// the same .env.override apart. Empty without the variable.
const std::string& RunSchema() {
  static const std::string schema = [] {
    const char* run = std::getenv("MOTHERDUCK_TEST_RUN");
    if (run == nullptr || *run == '\0') return std::string();
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    return "adbc_test_" + std::to_string(static_cast<int64_t>(std::time(nullptr))) + "_" +
           std::to_string(pid) + "_" + run;
  }();
  return schema;
}

// The schema this run works in, or main.
std::string TestSchema() { return RunSchema().empty() ? "main" : RunSchema(); }

// Another schema (or database) of this run's own, named after the run's
// schema, so concurrent runs never share it. MotherDuckTestDatabase drops it.
std::string TestSchema(std::string_view suffix) {
  if (RunSchema().empty()) return std::string(suffix);
  return RunSchema() + "_" + std::string(suffix);
}

std::string Upper(std::string value) {
  for (char& c : value) c = static_cast<char>(std::toupper(c));
  return value;
}

}  // namespace

class PostgresQuirks : public adbc_validation::DriverQuirks {
 public:
  // Leaves every other option at the driver's default.
  AdbcStatusCode SetupUri(struct AdbcDatabase* database, struct AdbcError* error) const {
    const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
    if (!uri) {
      ADD_FAILURE() << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    return AdbcDatabaseSetOption(database, "uri", uri, error);
  }

  AdbcStatusCode SetupDatabase(struct AdbcDatabase* database,
                               struct AdbcError* error) const override {
    const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
    if (!uri) {
      ADD_FAILURE() << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    RAISE_ADBC(SetupDatabase(database, uri, error));
    // Every connection starts in this run's schema.
    if (!RunSchema().empty()) {
      RAISE_ADBC(AdbcDatabaseSetOption(database, "motherduck.db_schema",
                                       TestSchema().c_str(), error));
    }
    return ADBC_STATUS_OK;
  }

  AdbcStatusCode SetupDatabase(struct AdbcDatabase* database, const char* uri,
                               struct AdbcError* error) const {
    return AdbcDatabaseSetOption(database, "uri", uri, error);
  }

  std::string QuoteIdentifier(std::string_view name) const override {
    std::string quoted = "\"";
    for (char c : name) {
      if (c == '"') quoted += '"';
      quoted += c;
    }
    return quoted + '"';
  }

  AdbcStatusCode DropTable(struct AdbcConnection* connection, const std::string& name,
                           struct AdbcError* error) const override {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "DROP TABLE IF EXISTS \"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  AdbcStatusCode DropTable(struct AdbcConnection* connection, const std::string& name,
                           const std::string& db_schema,
                           struct AdbcError* error) const override {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "DROP TABLE IF EXISTS \"" + db_schema + "\".\"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  // Bulk ingest isn't available (#31), so build the sample table with SQL.
  AdbcStatusCode CreateSampleTable(struct AdbcConnection* connection,
                                   const std::string& name,
                                   struct AdbcError* error) const override {
    return CreateSampleTableSql(connection, QuoteIdentifier(name), error);
  }

  AdbcStatusCode CreateSampleTable(struct AdbcConnection* connection,
                                   const std::string& name, const std::string& schema,
                                   struct AdbcError* error) const override {
    return CreateSampleTableSql(
        connection, QuoteIdentifier(schema) + "." + QuoteIdentifier(name), error);
  }

  AdbcStatusCode CreateSampleTableSql(struct AdbcConnection* connection,
                                      const std::string& table,
                                      struct AdbcError* error) const {
    for (const std::string& query :
         {"CREATE TABLE " + table + " (int64s BIGINT, strings TEXT)",
          "INSERT INTO " + table + " VALUES (42, 'foo'), (-42, NULL), (NULL, '')"}) {
      Handle<struct AdbcStatement> statement;
      RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));
      RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
      RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
      RAISE_ADBC(AdbcStatementRelease(&statement.value, error));
    }
    return ADBC_STATUS_OK;
  }

  AdbcStatusCode DropTempTable(struct AdbcConnection* connection, const std::string& name,
                               struct AdbcError* error) const override {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "DROP TABLE IF EXISTS pg_temp . \"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  AdbcStatusCode DropView(struct AdbcConnection* connection, const std::string& name,
                          struct AdbcError* error) const override {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "DROP VIEW IF EXISTS \"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  AdbcStatusCode EnsureDbSchema(struct AdbcConnection* connection,
                                const std::string& name,
                                struct AdbcError* error) const override {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "CREATE SCHEMA IF NOT EXISTS \"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  std::string BindParameter(int index) const override {
    return "$" + std::to_string(index + 1);
  }

  ArrowType IngestSelectRoundTripType(ArrowType ingest_type) const override {
    switch (ingest_type) {
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_UINT8:
        return NANOARROW_TYPE_INT16;
      case NANOARROW_TYPE_UINT16:
        return NANOARROW_TYPE_INT32;
      case NANOARROW_TYPE_UINT32:
      case NANOARROW_TYPE_UINT64:
        return NANOARROW_TYPE_INT64;
      case NANOARROW_TYPE_HALF_FLOAT:
        return NANOARROW_TYPE_FLOAT;
      case NANOARROW_TYPE_DURATION:
        return NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO;
      case NANOARROW_TYPE_LARGE_STRING:
      case NANOARROW_TYPE_STRING_VIEW:
        return NANOARROW_TYPE_STRING;
      case NANOARROW_TYPE_LARGE_BINARY:
      case NANOARROW_TYPE_FIXED_SIZE_BINARY:
      case NANOARROW_TYPE_BINARY_VIEW:
        return NANOARROW_TYPE_BINARY;
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256:
        return NANOARROW_TYPE_STRING;
      default:
        return ingest_type;
    }
  }

  std::optional<std::string> PrimaryKeyTableDdl(std::string_view name) const override {
    std::string ddl = "CREATE TABLE ";
    ddl += name;
    ddl += " (id INTEGER PRIMARY KEY)";
    return ddl;
  }

  std::optional<std::string> PrimaryKeyIngestTableDdl(
      std::string_view name) const override {
    // The test relies on an auto-generated key, which needs SERIAL or a
    // sequence; the Postgres endpoint supports neither.
    return std::nullopt;
  }

  std::optional<std::string> CompositePrimaryKeyTableDdl(
      std::string_view name) const override {
    std::string ddl = "CREATE TABLE ";
    ddl += name;
    ddl += " (id_primary_col1 INTEGER, id_primary_col2 INTEGER,";
    ddl += " PRIMARY KEY (id_primary_col1, id_primary_col2));";
    return ddl;
  }

  std::optional<std::string> ForeignKeyChildTableDdl(
      std::string_view child_name, std::string_view parent_name_1,
      std::string_view parent_name_2) const override {
    std::string ddl = "CREATE TABLE ";
    ddl += child_name;
    ddl += " (id_child_col1 INTEGER PRIMARY KEY, ";
    ddl += " id_child_col2 INTEGER, ";
    ddl += " id_child_col3 INTEGER, ";
    ddl += " FOREIGN KEY (id_child_col3) REFERENCES ";
    ddl += parent_name_1;
    ddl += " (id),";
    ddl += " FOREIGN KEY (id_child_col1, id_child_col2) REFERENCES ";
    ddl += parent_name_2;
    ddl += " (id_primary_col1, id_primary_col2))";
    return ddl;
  }

  // The database the test URI connects to.
  std::string catalog() const override {
    const char* catalog = std::getenv("MOTHERDUCK_TEST_CATALOG");
    if (!catalog) {
      ADD_FAILURE() << "Must provide env var MOTHERDUCK_TEST_CATALOG";
      return "";
    }
    return catalog;
  }
  std::string db_schema() const override { return TestSchema(); }
  std::string secondary_db_schema() const override {
    if (RunSchema().empty()) return adbc_validation::DriverQuirks::secondary_db_schema();
    return TestSchema("other");
  }

  // No COPY FROM STDIN yet (#31).
  bool supports_bulk_ingest(const char* mode) const override { return false; }
  // MotherDuck supports database.schema.table names.
  bool supports_bulk_ingest_catalog() const override { return true; }
  bool supports_bulk_ingest_db_schema() const override { return true; }
  // The Postgres endpoint rejects CREATE TEMP TABLE.
  bool supports_bulk_ingest_temporary() const override { return false; }
  bool supports_cancel() const override { return true; }
  bool supports_execute_schema() const override { return true; }
  std::optional<adbc_validation::SqlInfoValue> supports_get_sql_info(
      uint32_t info_code) const override {
    switch (info_code) {
      case ADBC_INFO_DRIVER_ADBC_VERSION:
        return ADBC_VERSION_1_1_0;
      case ADBC_INFO_DRIVER_NAME:
        return "ADBC MotherDuck Driver";
      case ADBC_INFO_DRIVER_VERSION:
        return ADBC_DRIVER_MOTHERDUCK_VERSION;
      case ADBC_INFO_VENDOR_NAME:
        return "MotherDuck";
      default:
        return std::nullopt;
    }
  }
  bool supports_metadata_current_catalog() const override { return true; }
  bool supports_metadata_current_db_schema() const override { return true; }
  // Statistics come from pg_stats, which DuckDB doesn't have.
  bool supports_statistics() const override { return false; }
};

// Whether name is a test run's schema or database, or one named after it, from
// a run that started a day or more before now (a Unix time): one an
// interrupted run left behind. Runs name them adbc_test_<time>_<pid>_...
// (RunSchema); earlier versions of the tests named them
// adbc_<time>_..._<os>_<arch>[_...]. Databases from the database-per-run
// setup, adbc_ci_<run>_..., carry no time, so they can't be told apart from a
// live run's and are left alone. validation/tests/naming.py has the same rule;
// keep the two, and their tests, in step.
bool IsStaleTestName(const std::string& name, int64_t now) {
  static const std::regex kCurrent("^adbc_test_([0-9]{10})_[0-9]+_.+$");
  static const std::regex kEarlier(
      "^adbc_([0-9]{10})_.+_(linux|macos|windows)_(amd64|arm64)(_.+)?$");
  std::smatch match;
  if (!std::regex_match(name, match, kCurrent) &&
      !std::regex_match(name, match, kEarlier)) {
    return false;
  }
  return std::stoll(match[1].str()) <= now - 86400;
}

// Gives the test run a schema of its own in the shared test database (see
// ci/scripts/pre-test.sh), and afterwards drops it, the other schemas and the
// databases named after it, and whatever runs left behind a day or more ago, so
// concurrent jobs and interrupted runs can't leave tables behind.
class MotherDuckTestDatabase : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!Configured()) return;
    Handle<struct AdbcDatabase> database;
    Handle<struct AdbcConnection> connection;
    ASSERT_NO_FATAL_FAILURE(Connect(&database.value, &connection.value));
    // Runs that start together race to create it the first time. The loser
    // fails, but by then the database exists, so a second try succeeds.
    const std::string create = "CREATE DATABASE IF NOT EXISTS " + catalog_;
    Handle<struct AdbcError> ignored;
    if (Run(&connection.value, create, &ignored.value) != ADBC_STATUS_OK) {
      ASSERT_NO_FATAL_FAILURE(Execute(&connection.value, create));
    }
    ASSERT_NO_FATAL_FAILURE(Execute(&connection.value, "USE " + catalog_));
    ASSERT_NO_FATAL_FAILURE(DropLeftovers(&connection.value));
    ASSERT_NO_FATAL_FAILURE(
        Execute(&connection.value, "CREATE SCHEMA " + quirks_.QuoteIdentifier(schema_)));
  }

  void TearDown() override {
    if (!Configured()) return;
    Handle<struct AdbcDatabase> database;
    Handle<struct AdbcConnection> connection;
    ASSERT_NO_FATAL_FAILURE(Connect(&database.value, &connection.value));
    ASSERT_NO_FATAL_FAILURE(Execute(&connection.value, "USE " + catalog_));
    ASSERT_NO_FATAL_FAILURE(DropLeftovers(&connection.value));
  }

 private:
  bool Configured() {
    const char* setup_uri = std::getenv("MOTHERDUCK_TEST_SETUP_URI");
    const char* catalog = std::getenv("MOTHERDUCK_TEST_CATALOG");
    if (!setup_uri || !catalog || RunSchema().empty()) return false;
    setup_uri_ = setup_uri;
    catalog_name_ = catalog;
    catalog_ = quirks_.QuoteIdentifier(catalog);
    schema_ = RunSchema();
    return true;
  }

  // A workspace-mode connection to md:, which can create and drop databases.
  void Connect(struct AdbcDatabase* database, struct AdbcConnection* connection) {
    Handle<struct AdbcError> error;
    ASSERT_THAT(AdbcDatabaseNew(database, &error.value), IsOkStatus(&error.value));
    ASSERT_THAT(quirks_.SetupDatabase(database, setup_uri_.c_str(), &error.value),
                IsOkStatus(&error.value));
    // Single attach mode can't connect to md:.
    ASSERT_THAT(AdbcDatabaseSetOption(database, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      "workspace", &error.value),
                IsOkStatus(&error.value));
    ASSERT_THAT(AdbcDatabaseInit(database, &error.value), IsOkStatus(&error.value));
    ASSERT_THAT(AdbcConnectionNew(connection, &error.value), IsOkStatus(&error.value));
    ASSERT_THAT(AdbcConnectionInit(connection, database, &error.value),
                IsOkStatus(&error.value));
  }

  // Drops, in the current database, this run's schemas, and in the account,
  // the databases named after them, along with any schema or database an
  // interrupted run left behind (IsStaleTestName).
  void DropLeftovers(struct AdbcConnection* connection) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto own = [&](const std::string& name) {
      return name == schema_ || name.rfind(schema_ + "_", 0) == 0;
    };
    // This run's own objects must go. Stale ones belong to a dead run, and
    // concurrent runs may drop the same one at once, which MotherDuck rejects
    // as a write-write conflict for all but one, so their drops are best-effort.
    auto drop = [&](const std::string& name, const std::string& query) {
      if (own(name)) {
        ASSERT_NO_FATAL_FAILURE(Execute(connection, query));
      } else if (IsStaleTestName(name, now)) {
        Handle<struct AdbcError> error;
        if (Run(connection, query, &error.value) != ADBC_STATUS_OK) {
          // Only a conflict means another run got there first.
          const std::string message = error.value.message ? error.value.message : "";
          if (message.find("onflict") == std::string::npos) {
            ADD_FAILURE() << "Failed to drop a stale leftover: " << message;
          }
        }
      }
    };

    std::vector<std::vector<std::string>> schemas;
    ASSERT_NO_FATAL_FAILURE(Query(connection,
                                  "SELECT schema_name FROM duckdb_schemas() "
                                  "WHERE database_name = current_database()",
                                  &schemas));
    for (const auto& row : schemas) {
      ASSERT_NO_FATAL_FAILURE(drop(row[0], "DROP SCHEMA IF EXISTS " + catalog_ + "." +
                                               quirks_.QuoteIdentifier(row[0]) +
                                               " CASCADE"));
    }

    std::vector<std::vector<std::string>> databases;
    ASSERT_NO_FATAL_FAILURE(
        Query(connection, "SELECT database_name FROM duckdb_databases()", &databases));
    for (const auto& row : databases) {
      if (row[0] == catalog_name_) continue;
      ASSERT_NO_FATAL_FAILURE(
          drop(row[0], "DROP DATABASE IF EXISTS " + quirks_.QuoteIdentifier(row[0])));
    }
  }

  AdbcStatusCode Run(struct AdbcConnection* connection, const std::string& query,
                     struct AdbcError* error) {
    Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    return AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error);
  }

  void Execute(struct AdbcConnection* connection, const std::string& query) {
    Handle<struct AdbcError> error;
    ASSERT_THAT(Run(connection, query, &error.value), IsOkStatus(&error.value)) << query;
  }

  // The query's rows, whose columns must all be strings.
  void Query(struct AdbcConnection* connection, const std::string& query,
             std::vector<std::vector<std::string>>* rows) {
    Handle<struct AdbcError> error;
    Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(connection, &statement.value, &error.value),
                IsOkStatus(&error.value));
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), &error.value),
                IsOkStatus(&error.value));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, &reader.stream.value, nullptr,
                                          &error.value),
                IsOkStatus(&error.value))
        << query;
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    while (true) {
      ASSERT_NO_FATAL_FAILURE(reader.Next());
      if (reader.array->release == nullptr) break;
      for (int64_t i = 0; i < reader.array->length; i++) {
        std::vector<std::string>& row = rows->emplace_back();
        for (int64_t c = 0; c < reader.array_view->n_children; c++) {
          const struct ArrowStringView value =
              ArrowArrayViewGetStringUnsafe(reader.array_view->children[c], i);
          row.emplace_back(value.data, value.size_bytes);
        }
      }
    }
  }

  PostgresQuirks quirks_;
  std::string setup_uri_;
  std::string catalog_name_;
  std::string catalog_;
  std::string schema_;
};

[[maybe_unused]] static const auto* kMotherDuckTestDatabase =
    ::testing::AddGlobalTestEnvironment(new MotherDuckTestDatabase);

// The cleanup drops whatever IsStaleTestName accepts, from a shared account, so
// pin down what it accepts. validation/tests/test_naming.py checks the Python
// copy against the same names.
TEST(MotherDuckTestNamesTest, IsStaleTestName) {
  const int64_t now = 1800000000;
  // A run that's still going, and the schemas and databases named after it.
  EXPECT_FALSE(IsStaleTestName("adbc_test_1799990000_123_local_1_macos_arm64", now));
  EXPECT_FALSE(
      IsStaleTestName("adbc_test_1799990000_123_local_1_macos_arm64_other_db", now));
  // Interrupted runs, a day or more ago.
  EXPECT_TRUE(IsStaleTestName("adbc_test_1799913600_123_local_1_macos_arm64", now));
  EXPECT_TRUE(IsStaleTestName("adbc_test_1700000000_12_backup", now));
  EXPECT_TRUE(IsStaleTestName("adbc_1700000000_local_1_linux_amd64_99", now));
  EXPECT_TRUE(IsStaleTestName("adbc_1700000000_1_1_test_windows_amd64_7_other_db", now));
  // Everything else stays.
  EXPECT_FALSE(IsStaleTestName("adbc_driver_tests", now));
  EXPECT_FALSE(IsStaleTestName("adbc_1000000000_unrelated", now));
  EXPECT_FALSE(IsStaleTestName("adbc_ci_18473920123_1_test_linux_amd64", now));
  EXPECT_FALSE(IsStaleTestName("adbc_test_", now));
  EXPECT_FALSE(IsStaleTestName("adbc_test_1700000000_12_", now));
  EXPECT_FALSE(IsStaleTestName("my_adbc_test_1700000000_12_x", now));
  EXPECT_FALSE(IsStaleTestName("main", now));
}

class PostgresDatabaseTest : public ::testing::Test,
                             public adbc_validation::DatabaseTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override { return &quirks_; }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

 protected:
  PostgresQuirks quirks_;
};
ADBCV_TEST_DATABASE(PostgresDatabaseTest)

TEST_F(PostgresDatabaseTest, UseCopyOption) {
  const char* key = "adbc.postgresql.use_copy";
  std::optional<std::string> use_copy;

  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  adbc_validation::Handle<struct AdbcStatement> statement;

  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, key, "true", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  use_copy = adbc_validation::DatabaseGetOption(&database.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("true"s));

  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::ConnectionGetOption(&connection.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("true"s));

  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::StatementGetOption(&statement.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("true"s));
  ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionSetOption(&connection.value, key, "false", &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::ConnectionGetOption(&connection.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("false"s));

  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::StatementGetOption(&statement.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("false"s));
  ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
}

// The endpoint rejects binary COPY TO STDOUT, so the default is off (#11).
TEST_F(PostgresDatabaseTest, UseCopyOptionDefault) {
  const char* key = "adbc.postgresql.use_copy";
  std::optional<std::string> use_copy;

  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  adbc_validation::Handle<struct AdbcStatement> statement;

  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupUri(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  use_copy = adbc_validation::DatabaseGetOption(&database.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("false"s));

  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::ConnectionGetOption(&connection.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("false"s));

  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  use_copy = adbc_validation::StatementGetOption(&statement.value, key, &error);
  EXPECT_THAT(use_copy, ::testing::Optional("false"s));
  ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
}

// https://github.com/adbc-drivers/motherduck/issues/10
TEST_F(PostgresDatabaseTest, MotherDuckDefaultsInit) {
  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupUri(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
}

// The test URI with a bad password in its authority, which takes precedence over
// MOTHERDUCK_TOKEN. The URI may have no user. False for a URI with no authority,
// such as md:my_db, which can't carry a password; callers use the password option.
static bool UriWithBadPassword(const char* uri, std::string* bad_uri) {
  *bad_uri = uri;
  size_t scheme_end = bad_uri->find("://");
  if (scheme_end == std::string::npos) return false;
  size_t authority = scheme_end + 3;
  size_t end = bad_uri->find_first_of("/?", authority);
  size_t at = bad_uri->find('@', authority);
  if (at != std::string::npos && at < end) {
    bad_uri->insert(at, ":invalid");
  } else {
    bad_uri->insert(authority, "postgres:invalid@");
  }
  return true;
}

TEST_F(PostgresDatabaseTest, InitRejectsBadCredentials) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  // A bad password, in the URI or the password option, takes precedence over
  // MOTHERDUCK_TOKEN.
  std::string bad_uri;
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  if (UriWithBadPassword(uri, &bad_uri)) {
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri", bad_uri.c_str(), &error),
                IsOkStatus(&error));
  } else {
    // A URI with no authority, such as md:my_db, can't carry a password.
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri", uri, &error),
                IsOkStatus(&error));
    ASSERT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_OPTION_PASSWORD, "invalid", &error),
        IsOkStatus(&error));
  }
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error),
              IsStatus(ADBC_STATUS_IO, &error));
}

// A uniquely named file in the temp directory, removed when the object is.
// A uniquely named file in the temp directory holding contents, readable only
// by its owner from the moment it exists, and removed when the object is. The
// path is empty if the file couldn't be created and written in full.
class ScopedTempFile {
 public:
  ScopedTempFile(std::string_view prefix, std::string_view contents) {
#ifdef _WIN32
    // The temp directory is per user on Windows.
    std::random_device random;
    path_ = std::filesystem::temp_directory_path() /
            (std::string(prefix) + "_" + std::to_string(random()));
    std::ofstream out(path_, std::ios::binary);
    out << contents;
    out.close();
    if (!out) path_.clear();
#else
    // mkstemp creates the file exclusively, with mode 0600.
    std::string name =
        (std::filesystem::temp_directory_path() / (std::string(prefix) + "_XXXXXX"))
            .string();
    int fd = mkstemp(name.data());
    if (fd < 0) return;
    path_ = name;
    size_t written = 0;
    while (written < contents.size()) {
      ssize_t n = write(fd, contents.data() + written, contents.size() - written);
      if (n <= 0) break;
      written += static_cast<size_t>(n);
    }
    if (close(fd) != 0 || written < contents.size()) {
      std::filesystem::remove(path_);
      path_.clear();
    }
#endif
  }
  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST_F(PostgresDatabaseTest, PasswordOption) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  const char* token = std::getenv("MOTHERDUCK_TOKEN");
  ASSERT_NE(token, nullptr) << "Must provide env var MOTHERDUCK_TOKEN";
  std::string bad_uri;
  const bool has_authority = UriWithBadPassword(uri, &bad_uri);

  // The option takes precedence over a password in the URI, which a URI with no
  // authority, such as md:my_db, can't carry.
  if (has_authority) {
    adbc_validation::Handle<struct AdbcDatabase> database;
    ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri", bad_uri.c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_OPTION_PASSWORD, token, &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  }
  // And over MOTHERDUCK_TOKEN.
  {
    adbc_validation::Handle<struct AdbcDatabase> database;
    ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupUri(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_OPTION_PASSWORD, "invalid", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseInit(&database.value, &error),
                IsStatus(ADBC_STATUS_IO, &error));
  }
}

TEST_F(PostgresDatabaseTest, IgnoresPostgresPasswords) {
  const char* motherduck_token = std::getenv("MOTHERDUCK_TOKEN");
  ASSERT_NE(motherduck_token, nullptr) << "Must provide env var MOTHERDUCK_TOKEN";
  const std::string token = motherduck_token;

  // A valid token in PGPASSWORD or a password file doesn't rescue a bad one.
  ScopedTempFile scoped_passfile("adbc_motherduck_pgpass", "*:*:*:*:" + token + "\n");
  const std::filesystem::path& passfile = scoped_passfile.path();
  ASSERT_FALSE(passfile.empty()) << "Couldn't create a password file";
  {
    ScopedEnv pgpassword("PGPASSWORD", token.c_str());
    ScopedEnv pgpassfile("PGPASSFILE", passfile.string().c_str());
    ScopedEnv bad_token("MOTHERDUCK_TOKEN", "invalid");
    adbc_validation::Handle<struct AdbcDatabase> database;
    ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupUri(&database.value, &error), IsOkStatus(&error));
    EXPECT_THAT(AdbcDatabaseInit(&database.value, &error),
                IsStatus(ADBC_STATUS_IO, &error));
  }
}

TEST_F(PostgresDatabaseTest, RegionOption) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  // Take the region from the test URI's host, then drop the host.
  std::string hostless = uri;
  size_t start = hostless.find('@');
  ASSERT_NE(start, std::string::npos) << "Test URI must be motherduck://user@host...";
  start++;
  size_t end = hostless.find_first_of(":/?", start);
  std::string host = hostless.substr(start, end - start);
  constexpr std::string_view kPrefix = "pg.";
  constexpr std::string_view kSuffix = "-aws.motherduck.com";
  if (host.size() <= kPrefix.size() + kSuffix.size() || host.rfind(kPrefix, 0) != 0 ||
      host.compare(host.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    GTEST_SKIP() << "Test host " << host << " isn't a MotherDuck region host";
  }
  std::string region =
      host.substr(kPrefix.size(), host.size() - kPrefix.size() - kSuffix.size());
  hostless.erase(start, end - start);

  // Only the region can supply the host and port.
  ScopedEnv pghost("PGHOST", "localhost");
  ScopedEnv pgport("PGPORT", "1");
  ScopedEnv pghostaddr("PGHOSTADDR", nullptr);
  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&database.value, hostless.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_REGION,
                                    region.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
}

TEST_F(PostgresDatabaseTest, SessionNameOption) {
  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
                                    "adbc session", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
}

TEST_F(PostgresDatabaseTest, ApplicationNameAndConnectTimeoutOptions) {
  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME,
                            "adbc test app", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value,
                                    ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "30", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
}

// The test URI without its query, and the root certificates its
// motherduck.tls_root_cert_path names.
static void SplitTestUri(const char* uri, std::string* base, std::string* rootcert) {
  *base = uri;
  base->resize(std::min(base->find('?'), base->size()));
  adbcpq::MotherduckDatabase database;
  struct AdbcError error = {};
  ASSERT_THAT(database.SetOption("uri", uri, &error), IsOkStatus(&error));
  char value[4096];
  size_t length = sizeof(value);
  ASSERT_THAT(database.GetOption(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, value,
                                 &length, &error),
              IsOkStatus(&error))
      << "Test URI must set motherduck.tls_root_cert_path";
  *rootcert = value;
}

// Each sslmode against MotherDuck's endpoint, which refuses plaintext: every
// mode that connects must use TLS. The modes and root certificates come from
// PGSSLMODE and PGSSLROOTCERT, which the driver still passes on, because a URI
// can't set them. The CA bundle comes from the test URI, since some libpq
// builds can't read the system trust store, or, without PGSSLROOTCERT, from the
// driver's search of the OS trust store.
TEST_F(PostgresDatabaseTest, SslmodeMatrix) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  std::string base, rootcert;
  ASSERT_NO_FATAL_FAILURE(SplitTestUri(uri, &base, &rootcert));

  struct Case {
    // Empty to leave it unset.
    const char* sslmode;
    std::string sslrootcert;
    bool connects;
  };
  const Case kCases[] = {
      // The driver finds the OS's roots and verifies the server.
      {"", "", true},
      {"verify-full", "", true},
      // The endpoint refuses plaintext.
      {"disable", "", false},
      // allow and prefer retry with TLS after that refusal.
      {"allow", "", true},
      {"prefer", "", true},
      {"require", "", true},
      {"verify-ca", rootcert, true},
      {"verify-full", rootcert, true},
      // libpq refuses anything but verify-full with the system trust store.
      {"require", "system", false},
      {"verify-ca", "system", false},
  };
  for (const Case& test_case : kCases) {
    ScopedEnv pgsslmode("PGSSLMODE",
                        *test_case.sslmode != '\0' ? test_case.sslmode : nullptr);
    ScopedEnv pgsslrootcert("PGSSLROOTCERT", !test_case.sslrootcert.empty()
                                                 ? test_case.sslrootcert.c_str()
                                                 : nullptr);
    const std::string& conn_uri = base;
    SCOPED_TRACE(std::string("sslmode=") + test_case.sslmode +
                 " sslrootcert=" + test_case.sslrootcert);
    adbcpq::MotherduckDatabase database;
    ASSERT_THAT(database.SetOption("uri", conn_uri.c_str(), &error), IsOkStatus(&error));
    PGconn* conn = nullptr;
    AdbcStatusCode status = database.Connect(&conn, &error);
    if (!test_case.connects) {
      EXPECT_THAT(status, IsStatus(ADBC_STATUS_IO, &error));
      if (error.release) error.release(&error);
      continue;
    }
    ASSERT_THAT(status, IsOkStatus(&error));
    EXPECT_EQ(PQsslInUse(conn), 1);
    if (*test_case.sslmode == '\0') {
      std::string sslmode;
      PQconninfoOption* used = PQconninfo(conn);
      for (PQconninfoOption* option = used; option->keyword != nullptr; option++) {
        if (std::string_view(option->keyword) == "sslmode" && option->val) {
          sslmode = option->val;
        }
      }
      PQconninfoFree(used);
      EXPECT_EQ(sslmode, "verify-full");
    }
    ASSERT_THAT(database.Disconnect(&conn, &error), IsOkStatus(&error));
  }
}

// motherduck.tls_root_cert_path verifies the server with the given
// file, and motherduck.tls_verify=false encrypts without verifying,
// against the real endpoint.
TEST_F(PostgresDatabaseTest, TlsOptions) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  std::string base, rootcert;
  ASSERT_NO_FATAL_FAILURE(SplitTestUri(uri, &base, &rootcert));

  // A file libpq can read but that holds no certificates.
  std::filesystem::path not_roots =
      std::filesystem::temp_directory_path() /
      ("motherduck_test_not_roots_" +
       std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".pem");
  std::ofstream(not_roots) << "not a certificate\n";

  struct Case {
    const char* option;
    std::string value;
    // The sslmode the connection used, or empty if it fails.
    const char* sslmode;
    // PGSSLROOTCERT, which tls_root_cert_path outranks. With tls_verify=false
    // libpq would still check the CA with it.
    const char* env_rootcert;
  };
  const std::string not_roots_path = not_roots.string();
  const Case kCases[] = {
      {ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, rootcert, "verify-full",
       not_roots_path.c_str()},
      {ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, not_roots_path, "", nullptr},
      {ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "false", "require", nullptr},
  };
  // The options outrank it.
  ScopedEnv pgsslmode("PGSSLMODE", "disable");
  for (const Case& test_case : kCases) {
    SCOPED_TRACE(std::string(test_case.option) + "=" + test_case.value);
    ScopedEnv pgsslrootcert("PGSSLROOTCERT", test_case.env_rootcert);
    adbcpq::MotherduckDatabase database;
    ASSERT_THAT(database.SetOption("uri", base.c_str(), &error), IsOkStatus(&error));
    ASSERT_THAT(database.SetOption(test_case.option, test_case.value.c_str(), &error),
                IsOkStatus(&error));
    PGconn* conn = nullptr;
    AdbcStatusCode status = database.Connect(&conn, &error);
    if (*test_case.sslmode == '\0') {
      EXPECT_THAT(status, IsStatus(ADBC_STATUS_IO, &error));
      if (error.release) error.release(&error);
      continue;
    }
    ASSERT_THAT(status, IsOkStatus(&error));
    EXPECT_EQ(PQsslInUse(conn), 1);
    std::string sslmode;
    PQconninfoOption* used = PQconninfo(conn);
    for (PQconninfoOption* option = used; option->keyword != nullptr; option++) {
      if (std::string_view(option->keyword) == "sslmode" && option->val) {
        sslmode = option->val;
      }
    }
    PQconninfoFree(used);
    EXPECT_EQ(sslmode, test_case.sslmode);
    ASSERT_THAT(database.Disconnect(&conn, &error), IsOkStatus(&error));
  }
  std::filesystem::remove(not_roots);
}

TEST_F(PostgresDatabaseTest, TypeResolverModeOptionIgnored) {
  const char* key = "adbc.postgresql.type_resolver_mode";

  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&database.value, &error), IsOkStatus(&error));
  // The warning is once per process, and no other test sets this option
  ::testing::internal::CaptureStderr();
  AdbcStatusCode status = AdbcDatabaseSetOption(&database.value, key, "server", &error);
  std::string warning = ::testing::internal::GetCapturedStderr();
  ASSERT_THAT(status, IsOkStatus(&error));
  EXPECT_THAT(warning, ::testing::HasSubstr("ignoring database option " +
                                            std::string(key) + "=server"));

  ::testing::internal::CaptureStderr();
  status = AdbcDatabaseSetOption(&database.value, key, "builtin", &error);
  warning = ::testing::internal::GetCapturedStderr();
  ASSERT_THAT(status, IsOkStatus(&error));
  EXPECT_EQ(warning, "");

  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
}

// https://github.com/adbc-drivers/motherduck/issues/11: a query returning rows
// works with the default options.
TEST_F(PostgresDatabaseTest, MotherDuckDefaultsSelect) {
  adbc_validation::Handle<struct AdbcDatabase> database;
  adbc_validation::Handle<struct AdbcConnection> connection;
  adbc_validation::Handle<struct AdbcStatement> statement;

  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupUri(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value, "SELECT 42 AS x", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_EQ(1, reader.array->length);
}

int Canary(const struct AdbcError*) { return 0; }

TEST_F(PostgresDatabaseTest, AdbcDriverBackwardsCompatibility) {
  struct AdbcDriver driver;
  std::memset(&driver, 0, ADBC_DRIVER_1_1_0_SIZE);
  driver.ErrorGetDetailCount = Canary;

  ASSERT_THAT(::AdbcDriverPostgresqlInit(ADBC_VERSION_1_0_0, &driver, &error),
              IsOkStatus(&error));

  ASSERT_EQ(Canary, driver.ErrorGetDetailCount);

  ASSERT_THAT(::AdbcDriverPostgresqlInit(424242, &driver, &error),
              IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error));
}

class PostgresConnectionTest : public ::testing::Test,
                               public adbc_validation::ConnectionTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override { return &quirks_; }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

 protected:
  PostgresQuirks quirks_;
};

TEST_F(PostgresConnectionTest, GetInfoMetadata) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  std::vector<uint32_t> info = {
      ADBC_INFO_DRIVER_NAME, ADBC_INFO_DRIVER_VERSION, ADBC_INFO_DRIVER_ADBC_VERSION,
      ADBC_INFO_VENDOR_NAME, ADBC_INFO_VENDOR_VERSION,
  };
  ASSERT_THAT(AdbcConnectionGetInfo(&connection, info.data(), info.size(),
                                    &reader.stream.value, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());

  std::vector<uint32_t> seen;
  while (true) {
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    if (!reader.array->release) break;

    for (int64_t row = 0; row < reader.array->length; row++) {
      ASSERT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[0], row));
      const uint32_t code =
          reader.array_view->children[0]->buffer_views[1].data.as_uint32[row];
      const uint32_t offset =
          reader.array_view->children[1]->buffer_views[1].data.as_int32[row];
      seen.push_back(code);

      struct ArrowArrayView* str_child = reader.array_view->children[1]->children[0];
      struct ArrowArrayView* int_child = reader.array_view->children[1]->children[2];
      switch (code) {
        case ADBC_INFO_DRIVER_NAME: {
          ArrowStringView val = ArrowArrayViewGetStringUnsafe(str_child, offset);
          EXPECT_EQ("ADBC MotherDuck Driver", std::string(val.data, val.size_bytes));
          break;
        }
        case ADBC_INFO_DRIVER_VERSION: {
          ArrowStringView val = ArrowArrayViewGetStringUnsafe(str_child, offset);
          EXPECT_EQ(ADBC_DRIVER_MOTHERDUCK_VERSION,
                    std::string(val.data, val.size_bytes));
          break;
        }
        case ADBC_INFO_VENDOR_NAME: {
          ArrowStringView val = ArrowArrayViewGetStringUnsafe(str_child, offset);
          EXPECT_EQ("MotherDuck", std::string(val.data, val.size_bytes));
          break;
        }
        case ADBC_INFO_VENDOR_VERSION:
          ADD_FAILURE() << "vendor version must not be reported";
          break;
        case ADBC_INFO_DRIVER_ADBC_VERSION: {
          EXPECT_EQ(ADBC_VERSION_1_1_0, ArrowArrayViewGetIntUnsafe(int_child, offset));
          break;
        }
        default:
          // Ignored
          break;
      }
    }
  }
  // The PostgreSQL server version is requested but not reported.
  ASSERT_THAT(seen, ::testing::UnorderedElementsAre(
                        ADBC_INFO_DRIVER_NAME, ADBC_INFO_DRIVER_VERSION,
                        ADBC_INFO_DRIVER_ADBC_VERSION, ADBC_INFO_VENDOR_NAME));
}

TEST_F(PostgresConnectionTest, GetObjectsGetCatalogs) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_CATALOGS, nullptr, nullptr,
                               nullptr, nullptr, nullptr, &reader.stream.value, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";

  const std::string catalog = quirks()->catalog();
  struct AdbcGetObjectsCatalog* cat =
      InternalAdbcGetObjectsDataGetCatalogByName(*get_objects_data, catalog.c_str());
  ASSERT_NE(cat, nullptr) << "catalog " << catalog << " not found";
}

namespace {
AdbcStatusCode ExecuteSql(struct AdbcDatabase* database, const std::string& query,
                          struct AdbcError* error) {
  Handle<struct AdbcConnection> connection;
  Handle<struct AdbcStatement> statement;
  AdbcStatusCode status = AdbcConnectionNew(&connection.value, error);
  if (status != ADBC_STATUS_OK) return status;
  status = AdbcConnectionInit(&connection.value, database, error);
  if (status != ADBC_STATUS_OK) return status;
  status = AdbcStatementNew(&connection.value, &statement.value, error);
  if (status != ADBC_STATUS_OK) return status;
  status = AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error);
  if (status != ADBC_STATUS_OK) return status;
  return AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error);
}

// The catalogs GetObjects lists on a new connection to the database.
void ListCatalogs(struct AdbcDatabase* database, std::vector<std::string>* catalogs) {
  Handle<struct AdbcError> error;
  Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error.value),
              IsOkStatus(&error.value));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, database, &error.value),
              IsOkStatus(&error.value));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection.value, ADBC_OBJECT_DEPTH_CATALOGS,
                                       nullptr, nullptr, nullptr, nullptr, nullptr,
                                       &reader.stream.value, &error.value),
              IsOkStatus(&error.value));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";
  for (int i = 0; i < (*get_objects_data)->n_catalogs; i++) {
    const struct ArrowStringView name = (*get_objects_data)->catalogs[i]->catalog_name;
    catalogs->emplace_back(name.data, name.size_bytes);
  }
}

// The schemas GetObjects lists in a catalog on a new connection to the database.
void ListSchemas(struct AdbcDatabase* database, const std::string& catalog,
                 std::vector<std::string>* schemas) {
  Handle<struct AdbcError> error;
  Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error.value),
              IsOkStatus(&error.value));
  ASSERT_THAT(AdbcConnectionInit(&connection.value, database, &error.value),
              IsOkStatus(&error.value));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection.value, ADBC_OBJECT_DEPTH_DB_SCHEMAS,
                                       catalog.c_str(), nullptr, nullptr, nullptr,
                                       nullptr, &reader.stream.value, &error.value),
              IsOkStatus(&error.value));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";
  struct AdbcGetObjectsCatalog* cat =
      InternalAdbcGetObjectsDataGetCatalogByName(*get_objects_data, catalog.c_str());
  ASSERT_NE(cat, nullptr) << "catalog " << catalog << " not found";
  for (int i = 0; i < cat->n_db_schemas; i++) {
    const struct ArrowStringView name = cat->catalog_db_schemas[i]->db_schema_name;
    schemas->emplace_back(name.data, name.size_bytes);
  }
}
}  // namespace

// With attach_mode=single (the default), other databases in the account aren't
// catalogs; with workspace, they are.
TEST_F(PostgresConnectionTest, GetObjectsGetCatalogsAttachMode) {
  Handle<struct AdbcDatabase> workspace;
  ASSERT_THAT(AdbcDatabaseNew(&workspace.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&workspace.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&workspace.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                    "workspace", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&workspace.value, &error), IsOkStatus(&error));

  const std::string catalog = quirks()->catalog();
  // Named after the run's schema, so concurrent runs don't share it, and
  // dropped with the run's leftovers if the test doesn't get to drop it.
  const std::string other =
      !RunSchema().empty() ? TestSchema("other_db") : catalog + "_other";
  const std::string drop = "DROP DATABASE IF EXISTS " + quirks_.QuoteIdentifier(other);
  ASSERT_THAT(ExecuteSql(&workspace.value, drop, &error), IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&workspace.value,
                         "CREATE DATABASE " + quirks_.QuoteIdentifier(other), &error),
              IsOkStatus(&error));

  // attach_mode in the URI overrides the default, and the option overrides the
  // URI.
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  std::string workspace_uri = uri;
  workspace_uri += std::strchr(uri, '?') ? "&" : "?";
  workspace_uri += "motherduck.attach_mode=workspace";
  Handle<struct AdbcDatabase> uri_workspace;
  Handle<struct AdbcDatabase> option_single;
  for (struct AdbcDatabase* db : {&uri_workspace.value, &option_single.value}) {
    ASSERT_THAT(AdbcDatabaseNew(db, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupDatabase(db, workspace_uri.c_str(), &error),
                IsOkStatus(&error));
  }
  ASSERT_THAT(AdbcDatabaseSetOption(&option_single.value,
                                    ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, "single", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&uri_workspace.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&option_single.value, &error), IsOkStatus(&error));

  std::vector<std::string> workspace_catalogs;
  std::vector<std::string> single_catalogs;
  std::vector<std::string> uri_workspace_catalogs;
  std::vector<std::string> option_single_catalogs;
  ListCatalogs(&workspace.value, &workspace_catalogs);
  ListCatalogs(&database, &single_catalogs);
  ListCatalogs(&uri_workspace.value, &uri_workspace_catalogs);
  ListCatalogs(&option_single.value, &option_single_catalogs);
  // Schemas are listed for every catalog, not only the connection's database.
  std::vector<std::string> other_schemas;
  ListSchemas(&workspace.value, other, &other_schemas);
  EXPECT_THAT(ExecuteSql(&workspace.value, drop, &error), IsOkStatus(&error));
  if (HasFatalFailure()) return;

  EXPECT_THAT(workspace_catalogs, ::testing::IsSupersetOf({catalog, other}));
  EXPECT_THAT(single_catalogs, ::testing::Contains(catalog));
  EXPECT_THAT(single_catalogs, ::testing::Not(::testing::Contains(other)));
  EXPECT_THAT(uri_workspace_catalogs, ::testing::IsSupersetOf({catalog, other}));
  EXPECT_THAT(option_single_catalogs, ::testing::Contains(catalog));
  EXPECT_THAT(option_single_catalogs, ::testing::Not(::testing::Contains(other)));
  EXPECT_THAT(other_schemas, ::testing::Contains("main"));
}

TEST_F(PostgresConnectionTest, GetObjectsGetDbSchemas) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_DB_SCHEMAS, nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       &reader.stream.value, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";

  struct AdbcGetObjectsSchema* schema = InternalAdbcGetObjectsDataGetSchemaByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str());
  ASSERT_NE(schema, nullptr) << "schema " << quirks()->db_schema() << " not found";

  // MotherDuck doesn't mark md_information_schema internal, but it isn't a
  // user database.
  EXPECT_EQ(InternalAdbcGetObjectsDataGetCatalogByName(*get_objects_data,
                                                       "md_information_schema"),
            nullptr);

  // DuckDB marks main internal in every database, so GetObjects filters on the
  // database. The system schemas live only in the internal system database.
  for (const char* system_schema : {"pg_catalog", "information_schema"}) {
    EXPECT_EQ(InternalAdbcGetObjectsDataGetSchemaByName(
                  *get_objects_data, quirks()->catalog().c_str(), system_schema),
              nullptr)
        << "unexpected schema " << system_schema;
  }
}

TEST_F(PostgresConnectionTest, GetObjectsSchemaFilterFindsTablesOutsideSearchPath) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  const std::string schema_name = TestSchema("adbc_get_objects_test");
  const std::string table_name = "schema_filter_table";

  // Ensure the schema is not part of the current search_path.
  ASSERT_THAT(
      AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                              quirks()->db_schema().c_str(), &error),
      IsOkStatus(&error));

  ASSERT_THAT(quirks()->EnsureDbSchema(&connection, schema_name, &error),
              IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, table_name, schema_name, &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    std::string create =
        "CREATE TABLE \"" + schema_name + "\".\"" + table_name + "\" (ints INT)";
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value, create.c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_TABLES, nullptr,
                                       schema_name.c_str(), nullptr, nullptr, nullptr,
                                       &reader.stream.value, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";

  const auto catalog = adbc_validation::ConnectionGetOption(
      &connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, &error);
  ASSERT_TRUE(catalog.has_value());

  struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, catalog->c_str(), schema_name.c_str(), table_name.c_str());
  ASSERT_NE(table, nullptr) << "could not find " << schema_name << "." << table_name
                            << " via GetObjects";
}

TEST_F(PostgresConnectionTest, GetObjectsGetAllFindsPrimaryKey) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_pkey_test", &error),
              IsOkStatus(&error));

  struct AdbcStatement statement;
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "CREATE TABLE adbc_pkey_test (ints INT, id INTEGER PRIMARY KEY)",
            &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                               nullptr, nullptr, nullptr, &reader.stream.value, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";

  struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
      "adbc_pkey_test");
  ASSERT_NE(table, nullptr) << "could not find adbc_pkey_test table";

  ASSERT_EQ(table->n_table_columns, 2);
  struct AdbcGetObjectsColumn* column = InternalAdbcGetObjectsDataGetColumnByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
      "adbc_pkey_test", "id");
  ASSERT_NE(column, nullptr) << "could not find id column on adbc_pkey_test table";
  auto xdbc_type_name =
      std::string(column->xdbc_type_name.data, column->xdbc_type_name.size_bytes);
  // GetObjects reports DuckDB's type names, not PostgreSQL's (int4).
  ASSERT_EQ(xdbc_type_name, "INTEGER");

  ASSERT_EQ(table->n_table_constraints, 1)
      << "expected 1 constraint on adbc_pkey_test table, found: "
      << table->n_table_constraints;

  struct AdbcGetObjectsConstraint* constraint =
      InternalAdbcGetObjectsDataGetConstraintByName(
          *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
          "adbc_pkey_test", "adbc_pkey_test_id_pkey");
  // DuckDB names a primary key <table>_<columns>_pkey.
  ASSERT_NE(constraint, nullptr) << "could not find adbc_pkey_test_id_pkey constraint";

  auto constraint_type = std::string(constraint->constraint_type.data,
                                     constraint->constraint_type.size_bytes);
  ASSERT_EQ(constraint_type, "PRIMARY KEY");
  ASSERT_EQ(constraint->n_column_names, 1)
      << "expected constraint adbc_pkey_test_id_pkey to be applied to 1 column, found: "
      << constraint->n_column_names;

  auto constraint_column_name =
      std::string(constraint->constraint_column_names[0].data,
                  constraint->constraint_column_names[0].size_bytes);
  ASSERT_EQ(constraint_column_name, "id");
}

TEST_F(PostgresConnectionTest, GetObjectsGetAllFindsForeignKey) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_fkey_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_fkey_test_base", &error),
              IsOkStatus(&error));

  struct AdbcStatement statement;
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement,
                                 "CREATE TABLE adbc_fkey_test_base (id1 INT, id2 INT, "
                                 "PRIMARY KEY (id1, id2))",
                                 &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement,
                    "CREATE TABLE adbc_fkey_test (fid1 INT, fid2 INT, "
                    "FOREIGN KEY (fid1, fid2) REFERENCES adbc_fkey_test_base(id1, id2))",
                    &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                               nullptr, nullptr, nullptr, &reader.stream.value, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetInfoData object";

  struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
      "adbc_fkey_test");
  ASSERT_NE(table, nullptr) << "could not find adbc_fkey_test table";
  ASSERT_EQ(table->n_table_constraints, 1)
      << "expected 1 constraint on adbc_fkey_test table, found: "
      << table->n_table_constraints;

  // DuckDB names a foreign key <table>_<columns>_<referenced columns>_fkey.
  const std::string search_name = "adbc_fkey_test_fid1_fid2_id1_id2_fkey";
  struct AdbcGetObjectsConstraint* constraint =
      InternalAdbcGetObjectsDataGetConstraintByName(
          *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
          "adbc_fkey_test", search_name.c_str());
  ASSERT_NE(constraint, nullptr) << "could not find " << search_name << " constraint";

  auto constraint_type = std::string(constraint->constraint_type.data,
                                     constraint->constraint_type.size_bytes);
  ASSERT_EQ(constraint_type, "FOREIGN KEY");
  ASSERT_EQ(constraint->n_column_names, 2)
      << "expected constraint adbc_fkey_test_fid1_fid2_id1_id2_fkey to be applied to 2 "
         "columns, "
         "found: "
      << constraint->n_column_names;

  for (auto i = 0; i < 2; i++) {
    auto str_vw = constraint->constraint_column_names[i];
    auto str = std::string(str_vw.data, str_vw.size_bytes);
    if (i == 0) {
      ASSERT_EQ(str, "fid1");
    } else if (i == 1) {
      ASSERT_EQ(str, "fid2");
    }
  }

  ASSERT_EQ(constraint->n_column_usages, 2)
      << "expected constraint adbc_fkey_test_fid1_fid2_id1_id2_fkey to have 2 usages, "
         "found: "
      << constraint->n_column_usages;

  for (auto i = 0; i < 2; i++) {
    struct AdbcGetObjectsUsage* usage = constraint->constraint_column_usages[i];
    auto catalog_str = std::string(usage->fk_catalog.data, usage->fk_catalog.size_bytes);
    ASSERT_EQ(catalog_str, quirks()->catalog());
    auto schema_str =
        std::string(usage->fk_db_schema.data, usage->fk_db_schema.size_bytes);
    ASSERT_EQ(schema_str, quirks()->db_schema());
    auto table_str = std::string(usage->fk_table.data, usage->fk_table.size_bytes);
    ASSERT_EQ(table_str, "adbc_fkey_test_base");

    auto column_str =
        std::string(usage->fk_column_name.data, usage->fk_column_name.size_bytes);
    if (i == 0) {
      ASSERT_EQ(column_str, "id1");
    } else if (i == 1) {
      ASSERT_EQ(column_str, "id2");
    }
  }
}

TEST_F(PostgresConnectionTest, GetObjectsTableTypesFilter) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  ASSERT_THAT(quirks()->DropView(&connection, "adbc_table_types_view_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_table_types_table_test", &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement.value,
            "CREATE TABLE adbc_table_types_table_test (id1 INT, id2 INT)", &error),
        IsOkStatus(&error));

    int64_t rows_affected = 0;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement.value, nullptr, &rows_affected, &error),
        IsOkStatus(&error));
  }

  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value,
                                         "CREATE VIEW adbc_table_types_view_test AS ( "
                                         "SELECT * FROM adbc_table_types_table_test)",
                                         &error),
                IsOkStatus(&error));
    int64_t rows_affected = 0;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement.value, nullptr, &rows_affected, &error),
        IsOkStatus(&error));
  }

  adbc_validation::StreamReader reader;
  std::vector<const char*> table_types = {"view", nullptr};
  ASSERT_THAT(AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr,
                                       nullptr, nullptr, table_types.data(), nullptr,
                                       &reader.stream.value, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_GT(reader.array->length, 0);

  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetInfoData object";

  struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
      "adbc_table_types_table_test");
  ASSERT_EQ(table, nullptr) << "unexpected table adbc_table_types_table_test found";

  struct AdbcGetObjectsTable* view = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
      "adbc_table_types_view_test");
  ASSERT_NE(view, nullptr) << "did not find view adbc_table_types_view_test";
}

TEST_F(PostgresConnectionTest, MetadataSetCurrentDbSchema) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  const std::string schema_name = TestSchema("testschema");
  const std::string quoted_schema = quirks()->QuoteIdentifier(schema_name);

  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement.value,
                    ("CREATE SCHEMA IF NOT EXISTS " + quoted_schema).c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value,
                                         ("CREATE TABLE IF NOT EXISTS " + quoted_schema +
                                          ".schematable (ints INT)")
                                             .c_str(),
                                         &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
  }

  adbc_validation::Handle<struct AdbcStatement> statement;
  ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
              IsOkStatus(&error));

  // Table does not exist in this schema
  error.vendor_code = ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA;
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement.value, "SELECT * FROM schematable", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  // 42P01 = table not found
  ASSERT_EQ("42P01", std::string_view(error.sqlstate, 5));
  ASSERT_NE(0, AdbcErrorGetDetailCount(&error));
  bool found = false;
  for (int i = 0; i < AdbcErrorGetDetailCount(&error); i++) {
    struct AdbcErrorDetail detail = AdbcErrorGetDetail(&error, i);
    if (std::strcmp(detail.key, "PG_DIAG_MESSAGE_PRIMARY") == 0) {
      found = true;
      std::string_view message(reinterpret_cast<const char*>(detail.value),
                               detail.value_length);
      ASSERT_THAT(message, ::testing::HasSubstr("schematable"));
    }
  }
  error.release(&error);
  ASSERT_TRUE(found) << "Did not find expected error detail";

  ASSERT_THAT(
      AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                              schema_name.c_str(), &error),
      IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement.value, "SELECT * FROM schematable", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
}

TEST_F(PostgresConnectionTest, MetadataSetCurrentDbSchemaInit) {
  // Regression test: setting the schema before Init (which Python does)
  const std::string schema_name = TestSchema("regtestschema");

  // 1. Create the schema
  {
    ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value,
                                         ("CREATE SCHEMA IF NOT EXISTS " +
                                          quirks()->QuoteIdentifier(schema_name))
                                             .c_str(),
                                         &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionRelease(&connection, &error), IsOkStatus(&error));
  }

  // 2. Initialize a connection with the schema
  {
    ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
    ASSERT_THAT(
        AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                                schema_name.c_str(), &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

    ASSERT_THAT(adbc_validation::ConnectionGetOption(
                    &connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, &error),
                ::testing::Optional(schema_name));
  }
}

TEST_F(PostgresConnectionTest, MetadataGetSchemaCaseSensitiveTable) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  // Create sample table
  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value,
                                         "DROP TABLE IF EXISTS \"Uppercase\"", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement.value, "CREATE TABLE \"Uppercase\" (ints INT, strs TEXT)", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  // Check its schema
  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, nullptr, nullptr, "Uppercase",
                                           schema.get(), &error),
              IsOkStatus(&error));

  ASSERT_NE(schema->release, nullptr);
  ASSERT_STREQ(schema->format, "+s");
  ASSERT_EQ(schema->n_children, 2);
  ASSERT_STREQ(schema->children[0]->format, "i");
  ASSERT_STREQ(schema->children[1]->format, "u");
  ASSERT_STREQ(schema->children[0]->name, "ints");
  ASSERT_STREQ(schema->children[1]->name, "strs");

  // Do we have to release the connection here?
}

// Names with dots, quotes and spaces resolve however they're cased, and the
// endpoint's ::regclass, which can't parse them, isn't involved (#13).
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaQuotedNames) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  auto execute = [&](const char* query) {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value, query, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  };
  const std::string odd_schema = TestSchema("Odd.Schema \"x\"");
  const std::string upper_odd_schema = Upper(odd_schema);
  const std::string quoted_odd_schema = quirks()->QuoteIdentifier(odd_schema);
  ASSERT_NO_FATAL_FAILURE(
      execute(("DROP SCHEMA IF EXISTS " + quoted_odd_schema + " CASCADE").c_str()));
  ASSERT_NO_FATAL_FAILURE(execute(("CREATE SCHEMA " + quoted_odd_schema).c_str()));
  ASSERT_NO_FATAL_FAILURE(
      execute(("CREATE TABLE " + quoted_odd_schema +
               ".\"Odd.Table \"\"y\"\"\" (ints INT, \"str col\" TEXT)")
                  .c_str()));

  // DuckDB identifiers are case-insensitive, so other cases find the table too.
  const std::string catalog = quirks()->catalog();
  std::string upper_catalog = catalog;
  for (char& c : upper_catalog) c = static_cast<char>(std::toupper(c));
  struct Name {
    const char* catalog;
    const char* db_schema;
    const char* table;
  };
  for (const Name& name : {
           Name{nullptr, odd_schema.c_str(), "Odd.Table \"y\""},
           Name{catalog.c_str(), odd_schema.c_str(), "Odd.Table \"y\""},
           Name{upper_catalog.c_str(), upper_odd_schema.c_str(), "odd.table \"Y\""},
       }) {
    SCOPED_TRACE(std::string(name.db_schema) + " / " + name.table);
    nanoarrow::UniqueSchema schema;
    ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, name.catalog, name.db_schema,
                                             name.table, schema.get(), &error),
                IsOkStatus(&error));
    ASSERT_EQ(schema->n_children, 2);
    ASSERT_STREQ(schema->children[0]->format, "i");
    ASSERT_STREQ(schema->children[0]->name, "ints");
    ASSERT_STREQ(schema->children[1]->format, "u");
    ASSERT_STREQ(schema->children[1]->name, "str col");
  }

  for (const Name& name : {
           // Not in the current schema.
           Name{nullptr, nullptr, "Odd.Table \"y\""},
           // Not in another catalog.
           Name{"no_such_catalog", odd_schema.c_str(), "Odd.Table \"y\""},
           // Not a relation at all.
           Name{nullptr, nullptr, "(SELECT CURRENT_TIME)"},
       }) {
    SCOPED_TRACE(name.table);
    nanoarrow::UniqueSchema schema;
    ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, name.catalog, name.db_schema,
                                             name.table, schema.get(), &error),
                IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  }

  ASSERT_NO_FATAL_FAILURE(
      execute(("DROP SCHEMA IF EXISTS " + quoted_odd_schema + " CASCADE").c_str()));
}

// DuckDB identifiers are case-insensitive, even quoted ones, so a lookup in
// another case finds the table.
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaIgnoresCase) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "MixedCase", &error), IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database, "CREATE TABLE \"MixedCase\" (ints INT)", &error),
              IsOkStatus(&error));

  const std::string upper_schema = Upper(TestSchema());
  for (const char* db_schema :
       {static_cast<const char*>(nullptr), upper_schema.c_str()}) {
    SCOPED_TRACE(db_schema ? db_schema : "(no schema)");
    nanoarrow::UniqueSchema schema;
    ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, nullptr, db_schema, "mixedcase",
                                             schema.get(), &error),
                IsOkStatus(&error));
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_STREQ(schema->children[0]->name, "ints");
  }
}

// An unqualified name resolves through the search path, which includes
// pg_catalog, as in a query.
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaSearchPath) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  for (const char* db_schema : {static_cast<const char*>(nullptr), "pg_catalog"}) {
    SCOPED_TRACE(db_schema ? db_schema : "(no schema)");
    nanoarrow::UniqueSchema schema;
    ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, nullptr, db_schema, "pg_class",
                                             schema.get(), &error),
                IsOkStatus(&error));
    EXPECT_GT(schema->n_children, 0);
  }
}

TEST_F(PostgresConnectionTest, MetadataGetTableSchemaSchemaNotFound) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, nullptr, "thisschemadoesnotexist",
                                           "t", schema.get(), &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
}

// GetTableSchema looks the table up in the given catalog.
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaCatalog) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "MixedCase", &error), IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database, "CREATE TABLE \"MixedCase\" (ints INT)", &error),
              IsOkStatus(&error));

  const std::string catalog = quirks()->catalog();
  const std::string test_schema = TestSchema();
  for (const char* db_schema : {static_cast<const char*>(nullptr), test_schema.c_str()}) {
    SCOPED_TRACE(db_schema ? db_schema : "(no schema)");
    nanoarrow::UniqueSchema schema;
    ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, catalog.c_str(), db_schema,
                                             "MixedCase", schema.get(), &error),
                IsOkStatus(&error));
    ASSERT_EQ(schema->n_children, 1);
  }

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, "thiscatalogdoesnotexist",
                                           nullptr, "MixedCase", schema.get(), &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
}

// With a catalog and no schema, GetTableSchema looks in the current schema of
// the current database, as an unqualified name does.
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaCatalogCurrentSchema) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  const std::string schema_name = TestSchema("adbc_current_schema_test");
  const std::string quoted_schema = quirks()->QuoteIdentifier(schema_name);
  ASSERT_THAT(ExecuteSql(&database,
                         "CREATE SCHEMA IF NOT EXISTS " + quoted_schema +
                             "; CREATE OR REPLACE TABLE " + quoted_schema +
                             ".only_here (ints INT)",
                         &error),
              IsOkStatus(&error));
  ASSERT_THAT(
      AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                              schema_name.c_str(), &error),
      IsOkStatus(&error));

  std::string catalog = quirks()->catalog();
  for (char& c : catalog) c = static_cast<char>(std::toupper(c));
  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, catalog.c_str(), nullptr,
                                           "only_here", schema.get(), &error),
              IsOkStatus(&error));
  ASSERT_EQ(schema->n_children, 1);
  EXPECT_STREQ(schema->children[0]->name, "ints");
}

// Every GetObjects filter ignores case, as DuckDB identifiers do, and a
// column filter keeps the constraints on a matching column.
TEST_F(PostgresConnectionTest, GetObjectsFiltersIgnoreCase) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "MixedCase", &error), IsOkStatus(&error));
  ASSERT_THAT(
      ExecuteSql(&database,
                 "CREATE TABLE \"MixedCase\" (id INTEGER PRIMARY KEY, ints INT)", &error),
      IsOkStatus(&error));

  const std::string catalog = quirks()->catalog();
  std::string upper_catalog = catalog;
  for (char& c : upper_catalog) c = static_cast<char>(std::toupper(c));

  struct Case {
    const char* column_filter;
    const char* column;
    int n_constraints;
  };
  for (const Case& test_case : {Case{"ID", "id", 1}, Case{"INTS", "ints", 0}}) {
    SCOPED_TRACE(test_case.column_filter);
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcConnectionGetObjects(
                    &connection, ADBC_OBJECT_DEPTH_ALL, upper_catalog.c_str(),
                    Upper(TestSchema()).c_str(), "mixedcase", nullptr,
                    test_case.column_filter, &reader.stream.value, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);

    auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
    ASSERT_NE(*get_objects_data, nullptr)
        << "could not initialize the AdbcGetObjectsData object";
    // GetObjects reports the names as stored.
    struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, catalog.c_str(), TestSchema().c_str(), "MixedCase");
    ASSERT_NE(table, nullptr) << "could not find " << TestSchema()
                              << ".MixedCase via GetObjects";
    ASSERT_EQ(table->n_table_columns, 1);
    const struct ArrowStringView name = table->table_columns[0]->column_name;
    EXPECT_EQ(std::string(name.data, name.size_bytes), test_case.column);
    EXPECT_EQ(table->n_table_constraints, test_case.n_constraints);
  }
}

// GetObjects reports UNIQUE and CHECK constraints along with keys, including a
// CHECK on no column.
TEST_F(PostgresConnectionTest, GetObjectsFindsUniqueAndCheck) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_unique_check_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database,
                         "CREATE TABLE adbc_unique_check_test (a INT UNIQUE, "
                         "b INT CHECK (b > 0), c INT, UNIQUE (c, a), "
                         "CHECK (random() >= 0))",
                         &error),
              IsOkStatus(&error));

  using Constraint = std::pair<std::string, std::vector<std::string>>;
  const Constraint unique_a{"UNIQUE", {"a"}};
  const Constraint unique_ca{"UNIQUE", {"c", "a"}};
  const Constraint check_b{"CHECK", {"b"}};
  // A CHECK on no column applies to the table, so every column filter keeps it.
  const Constraint check_table{"CHECK", {}};
  struct Case {
    const char* column_filter;
    std::vector<Constraint> expected;
  };
  for (const Case& test_case :
       {Case{nullptr, {unique_a, unique_ca, check_b, check_table}},
        Case{"%", {unique_a, unique_ca, check_b, check_table}},
        Case{"B", {check_b, check_table}}, Case{"c", {unique_ca, check_table}}}) {
    SCOPED_TRACE(test_case.column_filter ? test_case.column_filter : "(none)");
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                                 "adbc_unique_check_test", nullptr,
                                 test_case.column_filter, &reader.stream.value, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);

    auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
    ASSERT_NE(*get_objects_data, nullptr)
        << "could not initialize the AdbcGetObjectsData object";
    struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_unique_check_test");
    ASSERT_NE(table, nullptr) << "could not find adbc_unique_check_test table";

    std::vector<Constraint> constraints;
    for (int i = 0; i < table->n_table_constraints; i++) {
      const struct AdbcGetObjectsConstraint* constraint = table->table_constraints[i];
      std::vector<std::string> columns;
      for (int j = 0; j < constraint->n_column_names; j++) {
        columns.emplace_back(constraint->constraint_column_names[j].data,
                             constraint->constraint_column_names[j].size_bytes);
      }
      constraints.emplace_back(std::string(constraint->constraint_type.data,
                                           constraint->constraint_type.size_bytes),
                               std::move(columns));
    }
    EXPECT_THAT(constraints, ::testing::UnorderedElementsAreArray(test_case.expected));
  }
}

// The table types filter ignores case, takes "BASE TABLE" for a table, and
// excludes the other type's tables and columns.
TEST_F(PostgresConnectionTest, GetObjectsTableTypesIgnoreCase) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropView(&connection, "adbc_types_case_v", &error),
              IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_types_case_t", &error),
              IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database,
                         "CREATE TABLE adbc_types_case_t (id INT PRIMARY KEY); "
                         "CREATE VIEW adbc_types_case_v AS SELECT 1 AS x",
                         &error),
              IsOkStatus(&error));

  struct Case {
    std::vector<const char*> types;
    bool table;
    bool view;
  };
  for (Case test_case : {Case{{"TABLE"}, true, false}, Case{{"BASE TABLE"}, true, false},
                         Case{{"View"}, false, true}, Case{{"VIEW", "Table"}, true, true},
                         Case{{"nosuchtype"}, false, false}}) {
    std::string trace;
    for (const char* type : test_case.types) trace += std::string(type) + ";";
    SCOPED_TRACE(trace);
    test_case.types.push_back(nullptr);
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                                 "adbc_types_case_%", test_case.types.data(), nullptr,
                                 &reader.stream.value, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);

    auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
    ASSERT_NE(*get_objects_data, nullptr)
        << "could not initialize the AdbcGetObjectsData object";
    struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_types_case_t");
    struct AdbcGetObjectsTable* view = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_types_case_v");
    EXPECT_EQ(table != nullptr, test_case.table);
    EXPECT_EQ(view != nullptr, test_case.view);
    if (table) {
      EXPECT_EQ(table->n_table_columns, 1);
      EXPECT_EQ(table->n_table_constraints, 1);
    }
    if (view) EXPECT_EQ(view->n_table_columns, 1);
  }
}

// GetObjects lists the session's temporary objects under the temp catalog,
// and GetTableSchema finds them there.
TEST_F(PostgresConnectionTest, GetObjectsListsTempObjects) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  {
    // Temporary objects belong to the connection that creates them.
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement.value,
            "CREATE OR REPLACE TEMP VIEW adbc_temp_view_test AS SELECT 1 AS x", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr,
                                       nullptr, "adbc_temp_view_test", nullptr, nullptr,
                                       &reader.stream.value, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
  ASSERT_NE(*get_objects_data, nullptr)
      << "could not initialize the AdbcGetObjectsData object";
  struct AdbcGetObjectsTable* view = InternalAdbcGetObjectsDataGetTableByName(
      *get_objects_data, "temp", "main", "adbc_temp_view_test");
  ASSERT_NE(view, nullptr) << "could not find temp.main.adbc_temp_view_test";
  EXPECT_EQ(std::string(view->table_type.data, view->table_type.size_bytes), "view");
  EXPECT_EQ(view->n_table_columns, 1);

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&connection, "temp", nullptr,
                                           "adbc_temp_view_test", schema.get(), &error),
              IsOkStatus(&error));
  ASSERT_EQ(schema->n_children, 1);
}

// Connected to md:, the current catalog option reports md:. Passed back to
// GetTableSchema without a schema, it means the current database and schema.
TEST_F(PostgresConnectionTest, MetadataGetTableSchemaMdCatalog) {
  ASSERT_THAT(
      ExecuteSql(&database, "CREATE OR REPLACE TABLE adbc_md_catalog_test (ints INT)",
                 &error),
      IsOkStatus(&error));

  // The test URI with md: as its database.
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  ASSERT_NE(uri, nullptr) << "Must provide env var ADBC_POSTGRESQL_TEST_URI";
  std::string md_uri = uri;
  const size_t path = md_uri.find('/', md_uri.find("://") + 3);
  ASSERT_NE(path, std::string::npos) << "test URI has no database";
  const size_t query = md_uri.find('?', path);
  md_uri.replace(
      path + 1, (query == std::string::npos ? md_uri.size() : query) - path - 1, "md%3A");

  Handle<struct AdbcDatabase> md;
  ASSERT_THAT(AdbcDatabaseNew(&md.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&md.value, md_uri.c_str(), &error),
              IsOkStatus(&error));
  // Single attach mode can't connect to md:.
  ASSERT_THAT(AdbcDatabaseSetOption(&md.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                    "workspace", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&md.value, &error), IsOkStatus(&error));
  Handle<struct AdbcConnection> md_connection;
  ASSERT_THAT(AdbcConnectionNew(&md_connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&md_connection.value, &md.value, &error),
              IsOkStatus(&error));
  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&md_connection.value, &statement.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement.value,
                                 ("USE " + quirks_.QuoteIdentifier(quirks()->catalog()) +
                                  "." + quirks_.QuoteIdentifier(quirks()->db_schema()))
                                     .c_str(),
                                 &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  char current_catalog[256];
  size_t length = sizeof(current_catalog);
  ASSERT_THAT(AdbcConnectionGetOption(&md_connection.value,
                                      ADBC_CONNECTION_OPTION_CURRENT_CATALOG,
                                      current_catalog, &length, &error),
              IsOkStatus(&error));
  ASSERT_STREQ(current_catalog, "md:");

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcConnectionGetTableSchema(&md_connection.value, current_catalog, nullptr,
                                           "adbc_md_catalog_test", schema.get(), &error),
              IsOkStatus(&error));
  ASSERT_EQ(schema->n_children, 1);
  EXPECT_STREQ(schema->children[0]->name, "ints");
}

// GetObjects lists a view's columns, and GetTableSchema describes a view.
TEST_F(PostgresConnectionTest, MetadataViewColumns) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropView(&connection, "adbc_view_columns_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database,
                         "CREATE VIEW adbc_view_columns_test AS "
                         "SELECT 1::INTEGER AS id, 'x'::VARCHAR AS name",
                         &error),
              IsOkStatus(&error));

  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr,
                                         nullptr, "adbc_view_columns_test", nullptr,
                                         nullptr, &reader.stream.value, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);

    auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
    ASSERT_NE(*get_objects_data, nullptr)
        << "could not initialize the AdbcGetObjectsData object";
    struct AdbcGetObjectsTable* view = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_view_columns_test");
    ASSERT_NE(view, nullptr) << "could not find adbc_view_columns_test view";
    EXPECT_EQ(std::string(view->table_type.data, view->table_type.size_bytes), "view");
    ASSERT_EQ(view->n_table_columns, 2);
    struct AdbcGetObjectsColumn* column = InternalAdbcGetObjectsDataGetColumnByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_view_columns_test", "name");
    ASSERT_NE(column, nullptr) << "could not find name column on the view";
    EXPECT_EQ(column->ordinal_position, 2);
    EXPECT_EQ(std::string(column->xdbc_type_name.data, column->xdbc_type_name.size_bytes),
              "VARCHAR");
  }

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(
      AdbcConnectionGetTableSchema(&connection, nullptr, nullptr,
                                   "adbc_view_columns_test", schema.get(), &error),
      IsOkStatus(&error));
  ASSERT_EQ(schema->n_children, 2);
  EXPECT_STREQ(schema->children[0]->name, "id");
  EXPECT_STREQ(schema->children[1]->name, "name");
}

// GetObjects name filters are search patterns: % matches any run of
// characters and _ any one character.
TEST_F(PostgresConnectionTest, GetObjectsFilterPatterns) {
  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_pattern_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&database, "CREATE TABLE adbc_pattern_test (ints INT)", &error),
              IsOkStatus(&error));

  // n_columns is -1 when the table shouldn't be found.
  struct Case {
    const char* table_filter;
    const char* column_filter;
    int n_columns;
  };
  for (const Case& test_case :
       {Case{"adbc_pattern%", "i%", 1}, Case{"adbc_pattern_tes_", "int_", 1},
        Case{"adbc_pattern_te_", nullptr, -1}, Case{"adbc_pattern_test", "x%", 0}}) {
    SCOPED_TRACE(std::string(test_case.table_filter) + " / " +
                 (test_case.column_filter ? test_case.column_filter : "(none)"));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                                 test_case.table_filter, nullptr, test_case.column_filter,
                                 &reader.stream.value, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);

    auto get_objects_data = adbc_validation::GetObjectsReader{&reader.array_view.value};
    ASSERT_NE(*get_objects_data, nullptr)
        << "could not initialize the AdbcGetObjectsData object";
    struct AdbcGetObjectsTable* table = InternalAdbcGetObjectsDataGetTableByName(
        *get_objects_data, quirks()->catalog().c_str(), quirks()->db_schema().c_str(),
        "adbc_pattern_test");
    if (test_case.n_columns < 0) {
      EXPECT_EQ(table, nullptr);
    } else {
      ASSERT_NE(table, nullptr) << "could not find adbc_pattern_test via GetObjects";
      EXPECT_EQ(table->n_table_columns, test_case.n_columns);
    }
  }
}

TEST_F(PostgresConnectionTest, MetadataGetStatistics) {
  if (!quirks()->supports_statistics()) {
    GTEST_SKIP();
  }

  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  // Create sample table
  {
    adbc_validation::Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcStatementNew(&connection, &statement.value, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value,
                                         "DROP TABLE IF EXISTS statstable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement.value,
                                 "CREATE TABLE statstable (ints INT, strs TEXT)", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement.value,
            "INSERT INTO statstable VALUES (1, 'a'), (NULL, 'bcd'), (-5, NULL)", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement.value, "ANALYZE statstable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementRelease(&statement.value, &error), IsOkStatus(&error));
  }

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcConnectionGetStatistics(&connection, nullptr, quirks()->db_schema().c_str(),
                                  "statstable", 1, &reader.stream.value, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());

  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
      &reader.schema.value, {
                                {"catalog_name", NANOARROW_TYPE_STRING, true},
                                {"catalog_db_schemas", NANOARROW_TYPE_LIST, false},
                            }));

  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
      reader.schema->children[1]->children[0],
      {
          {"db_schema_name", NANOARROW_TYPE_STRING, true},
          {"db_schema_statistics", NANOARROW_TYPE_LIST, false},
      }));

  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
      reader.schema->children[1]->children[0]->children[1]->children[0],
      {
          {"table_name", NANOARROW_TYPE_STRING, false},
          {"column_name", NANOARROW_TYPE_STRING, true},
          {"statistic_key", NANOARROW_TYPE_INT16, false},
          {"statistic_value", NANOARROW_TYPE_DENSE_UNION, false},
          {"statistic_is_approximate", NANOARROW_TYPE_BOOL, false},
      }));

  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
      reader.schema->children[1]->children[0]->children[1]->children[0]->children[3],
      {
          {"int64", NANOARROW_TYPE_INT64, true},
          {"uint64", NANOARROW_TYPE_UINT64, true},
          {"float64", NANOARROW_TYPE_DOUBLE, true},
          {"binary", NANOARROW_TYPE_BINARY, true},
      }));

  std::vector<std::tuple<std::optional<std::string>, int16_t, int64_t>> seen;
  while (true) {
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    if (!reader.array->release) break;

    for (int64_t catalog_index = 0; catalog_index < reader.array->length;
         catalog_index++) {
      struct ArrowStringView catalog_name =
          ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], catalog_index);
      ASSERT_EQ(quirks()->catalog(),
                std::string_view(catalog_name.data,
                                 static_cast<int64_t>(catalog_name.size_bytes)));

      struct ArrowArrayView* catalog_db_schemas = reader.array_view->children[1];
      struct ArrowArrayView* schema_stats = catalog_db_schemas->children[0]->children[1];
      struct ArrowArrayView* stats =
          catalog_db_schemas->children[0]->children[1]->children[0];
      for (int64_t schema_index =
               ArrowArrayViewListChildOffset(catalog_db_schemas, catalog_index);
           schema_index <
           ArrowArrayViewListChildOffset(catalog_db_schemas, catalog_index + 1);
           schema_index++) {
        struct ArrowStringView schema_name = ArrowArrayViewGetStringUnsafe(
            catalog_db_schemas->children[0]->children[0], schema_index);
        ASSERT_EQ(quirks()->db_schema(),
                  std::string_view(schema_name.data,
                                   static_cast<int64_t>(schema_name.size_bytes)));

        for (int64_t stat_index =
                 ArrowArrayViewListChildOffset(schema_stats, schema_index);
             stat_index < ArrowArrayViewListChildOffset(schema_stats, schema_index + 1);
             stat_index++) {
          struct ArrowStringView table_name =
              ArrowArrayViewGetStringUnsafe(stats->children[0], stat_index);
          ASSERT_EQ("statstable",
                    std::string_view(table_name.data,
                                     static_cast<int64_t>(table_name.size_bytes)));
          std::optional<std::string> column_name;
          if (!ArrowArrayViewIsNull(stats->children[1], stat_index)) {
            struct ArrowStringView value =
                ArrowArrayViewGetStringUnsafe(stats->children[1], stat_index);
            column_name = std::string(value.data, value.size_bytes);
          }
          ASSERT_TRUE(ArrowArrayViewGetIntUnsafe(stats->children[4], stat_index));

          const int16_t stat_key = static_cast<int16_t>(
              ArrowArrayViewGetIntUnsafe(stats->children[2], stat_index));
          const int32_t offset =
              stats->children[3]->buffer_views[1].data.as_int32[stat_index];
          int64_t stat_value;
          switch (stat_key) {
            case ADBC_STATISTIC_AVERAGE_BYTE_WIDTH_KEY:
            case ADBC_STATISTIC_DISTINCT_COUNT_KEY:
            case ADBC_STATISTIC_NULL_COUNT_KEY:
            case ADBC_STATISTIC_ROW_COUNT_KEY:
              stat_value = static_cast<int64_t>(
                  std::round(100 * ArrowArrayViewGetDoubleUnsafe(
                                       stats->children[3]->children[2], offset)));
              break;
            default:
              continue;
          }
          seen.emplace_back(std::move(column_name), stat_key, stat_value);
        }
      }
    }
  }

  ASSERT_THAT(seen,
              ::testing::UnorderedElementsAreArray(
                  std::vector<std::tuple<std::optional<std::string>, int16_t, int64_t>>{
                      {"ints", ADBC_STATISTIC_AVERAGE_BYTE_WIDTH_KEY, 400},
                      {"strs", ADBC_STATISTIC_AVERAGE_BYTE_WIDTH_KEY, 300},
                      {"ints", ADBC_STATISTIC_NULL_COUNT_KEY, 100},
                      {"strs", ADBC_STATISTIC_NULL_COUNT_KEY, 100},
                      {"ints", ADBC_STATISTIC_DISTINCT_COUNT_KEY, 200},
                      {"strs", ADBC_STATISTIC_DISTINCT_COUNT_KEY, 200},
                      {std::nullopt, ADBC_STATISTIC_ROW_COUNT_KEY, 300},
                  }));
}

ADBCV_TEST_CONNECTION(PostgresConnectionTest)

class PostgresStatementTest : public ::testing::Test,
                              public adbc_validation::StatementTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override { return &quirks_; }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

  void TestSqlPrepareErrorParamCountMismatch() { GTEST_SKIP() << "Not yet implemented"; }

  void TestSqlPrepareSelectParams() { GTEST_SKIP() << "Not yet implemented"; }
  void TestSqlIngestColumnEscaping() { SKIP_WITHOUT_COPY_FROM_STDIN(); }
  void TestSqlIngestTableEscaping() { SKIP_WITHOUT_COPY_FROM_STDIN(); }

  void TestConcurrentStatements() {
    // TODO: refactor driver so that we read all the data as soon as
    // we ExecuteQuery() since that's how libpq already works - then
    // we can actually support concurrent statements (because there is
    // no concurrency)
    GTEST_SKIP() << "Not yet implemented";
  }

 protected:
  void ValidateIngestedTemporalData(struct ArrowArrayView* values, ArrowType type,
                                    enum ArrowTimeUnit unit,
                                    const char* timezone) override {
    switch (type) {
      case NANOARROW_TYPE_TIMESTAMP: {
        std::vector<std::optional<int64_t>> expected;
        switch (unit) {
          case (NANOARROW_TIME_UNIT_SECOND):
            expected.insert(expected.end(), {std::nullopt, -42000000, 0, 42000000});
            break;
          case (NANOARROW_TIME_UNIT_MILLI):
            expected.insert(expected.end(), {std::nullopt, -42000, 0, 42000});
            break;
          case (NANOARROW_TIME_UNIT_MICRO):
            expected.insert(expected.end(), {std::nullopt, -42, 0, 42});
            break;
          case (NANOARROW_TIME_UNIT_NANO):
            expected.insert(expected.end(), {std::nullopt, 0, 0, 0});
            break;
        }
        ASSERT_NO_FATAL_FAILURE(
            adbc_validation::CompareArray<std::int64_t>(values, expected));
        break;
      }
      case NANOARROW_TYPE_DURATION: {
        struct ArrowInterval neg_interval;
        struct ArrowInterval zero_interval;
        struct ArrowInterval pos_interval;

        ArrowIntervalInit(&neg_interval, type);
        ArrowIntervalInit(&zero_interval, type);
        ArrowIntervalInit(&pos_interval, type);

        neg_interval.months = 0;
        neg_interval.days = 0;
        zero_interval.months = 0;
        zero_interval.days = 0;
        pos_interval.months = 0;
        pos_interval.days = 0;

        switch (unit) {
          case (NANOARROW_TIME_UNIT_SECOND):
            neg_interval.ns = -42000000000;
            zero_interval.ns = 0;
            pos_interval.ns = 42000000000;
            break;
          case (NANOARROW_TIME_UNIT_MILLI):
            neg_interval.ns = -42000000;
            zero_interval.ns = 0;
            pos_interval.ns = 42000000;
            break;
          case (NANOARROW_TIME_UNIT_MICRO):
            neg_interval.ns = -42000;
            zero_interval.ns = 0;
            pos_interval.ns = 42000;
            break;
          case (NANOARROW_TIME_UNIT_NANO):
            // lower than us precision is lost
            neg_interval.ns = 0;
            zero_interval.ns = 0;
            pos_interval.ns = 0;
            break;
        }
        const std::vector<std::optional<ArrowInterval*>> expected = {
            std::nullopt, &neg_interval, &zero_interval, &pos_interval};
        ASSERT_NO_FATAL_FAILURE(
            adbc_validation::CompareArray<ArrowInterval*>(values, expected));
        break;
      }
      default:
        FAIL() << "ValidateIngestedTemporalData not implemented for type " << type;
    }
  }

  PostgresQuirks quirks_;
};
ADBCV_TEST_STATEMENT(PostgresStatementTest)

// The endpoint reports a missing catalog with a generic SQLSTATE; the driver
// still classifies it as NOT_FOUND.
TEST_F(PostgresStatementTest, MissingCatalogIsNotFound) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT * FROM thiscatalogdoesnotexist.main.t", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
}

// Every documented DuckDB type should resolve to a known type, not unnamed<oid:N>
TEST_F(PostgresStatementTest, MotherDuckTypesResolve) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement,
          "SELECT 1::TINYINT AS ti, 1::SMALLINT AS si, 1::INTEGER AS i, "
          "1::BIGINT AS bi, 1::HUGEINT AS hi, 1::UTINYINT AS uti, "
          "1::USMALLINT AS usi, 1::UINTEGER AS ui, 1::UBIGINT AS ub, "
          "1::UHUGEINT AS uhi, 1::BIGNUM AS bn, 1.5::DECIMAL(10, 2) AS d, "
          "1.5::FLOAT AS fl, 1.5::DOUBLE AS f, true AS bo, '101'::BIT AS bt, "
          "'x' AS v, 'x'::BLOB AS b, gen_random_uuid() AS u, '{}'::JSON AS j, "
          "DATE '2020-01-01' AS dt, TIME '12:00:00' AS tm, "
          "TIMETZ '12:00:00+01' AS ttz, '12:00:00'::TIME_NS AS tmns, "
          "TIMESTAMP '2020-01-01 00:00:00' AS tsl, "
          "TIMESTAMP_S '2020-01-01 00:00:00' AS tss, "
          "TIMESTAMP_MS '2020-01-01 00:00:00' AS tsms, "
          "TIMESTAMPTZ '2020-01-01 00:00:00+00' AS ts, INTERVAL 1 DAY AS iv, "
          "'POINT(1 2)'::GEOMETRY AS g, [1, 2] AS l, [1, 2, 3]::INTEGER[3] AS a, "
          "{'a': 1} AS s, ROW(1, 'a') AS r, MAP {'a': 1} AS m, "
          "union_value(n := 1) AS un, 1::VARIANT AS va, 'a'::ENUM('a', 'b') AS e, "
          "[[1, 2]] AS nl, '2020-01-01 00:00:00.123456789'::TIMESTAMP_NS AS tns, "
          "[{'a': 1}] AS ls",
          &error),
      IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  for (int64_t i = 0; i < reader.schema->n_children; i++) {
    const ArrowSchema* child = reader.schema->children[i];
    SCOPED_TRACE(child->name);
    // Unresolved OIDs are labelled unnamed<oid:N>
    ArrowStringView typname = {nullptr, 0};
    ASSERT_EQ(ArrowMetadataGetValue(child->metadata,
                                    ArrowCharView("ADBC:postgresql:typname"), &typname),
              NANOARROW_OK);
    if (typname.data != nullptr) {
      EXPECT_THAT(std::string(typname.data, static_cast<size_t>(typname.size_bytes)),
                  ::testing::Not(::testing::StartsWith("unnamed")));
    }
  }

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);

  // DuckDB-only types arrive as text
  const std::vector<std::pair<std::string, std::string>> expected_text = {
      {"s", "{'a': 1}"},
      {"m", "{a=1}"},
      {"e", "a"},
      {"nl", "[[1, 2]]"},
      {"tns", "2020-01-01 00:00:00.123456789"},
  };
  for (const auto& [name, expected] : expected_text) {
    SCOPED_TRACE(name);
    int64_t index = -1;
    for (int64_t i = 0; i < reader.schema->n_children; i++) {
      if (name == reader.schema->children[i]->name) index = i;
    }
    ASSERT_GE(index, 0);
    ASSERT_STREQ(reader.schema->children[index]->format, "u");
    ArrowStringView view =
        ArrowArrayViewGetStringUnsafe(reader.array_view->children[index], 0);
    EXPECT_EQ(std::string_view(view.data, static_cast<size_t>(view.size_bytes)),
              expected);
  }

  const int64_t ls_index = reader.schema->n_children - 1;
  ASSERT_STREQ(reader.schema->children[ls_index]->format, "+l");
  ASSERT_STREQ(reader.schema->children[ls_index]->children[0]->format, "u");
  const ArrowArrayView* ls_values = reader.array_view->children[ls_index]->children[0];
  ArrowStringView ls_view = ArrowArrayViewGetStringUnsafe(ls_values, 0);
  EXPECT_EQ(std::string_view(ls_view.data, static_cast<size_t>(ls_view.size_bytes)),
            "{'a': 1}");
}

// A query that quotes the typing error must still report its own error.
TEST_F(PostgresStatementTest, MotherDuckPrepareErrorQuotingTypingError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "SELECT missing_col -- ambiguous result column types", &error),
      IsOkStatus(&error));

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcStatementExecuteSchema(&statement, schema.get(), &error),
              ::testing::Not(IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error)));
  ASSERT_THAT(AdbcStatementGetParameterSchema(&statement, schema.get(), &error),
              ::testing::Not(IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error)));
}

// The cast retry depends on the endpoint's wording for this error. If this
// fails, update IsAmbiguousParameterError in motherduck_statement.cc.
TEST_F(PostgresStatementTest, MotherDuckAmbiguousParameterErrorWording) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1", &error),
              IsOkStatus(&error));
  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcStatementExecuteSchema(&statement, schema.get(), &error),
              IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error));
}

// Bind one row of an int32 and a string to query and return the result.
void ExecuteCastQuery(struct AdbcConnection* connection, struct AdbcStatement* statement,
                      const char* query, const char* string_value,
                      adbc_validation::StreamReader* reader,
                      nanoarrow::UniqueSchema* schema, struct AdbcError* error) {
  nanoarrow::UniqueSchema bind_schema;
  nanoarrow::UniqueArray bind;
  struct ArrowError na_error;
  ASSERT_THAT(
      adbc_validation::MakeSchema(
          bind_schema.get(), {{"a", NANOARROW_TYPE_INT32}, {"b", NANOARROW_TYPE_STRING}}),
      adbc_validation::IsOkErrno());
  ASSERT_THAT((adbc_validation::MakeBatch<int32_t, std::string>(
                  bind_schema.get(), bind.get(), &na_error, {1}, {string_value})),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementNew(connection, statement, error), IsOkStatus(error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(statement, query, error), IsOkStatus(error));
  ASSERT_THAT(AdbcStatementBind(statement, bind.get(), bind_schema.get(), error),
              IsOkStatus(error));
  ASSERT_THAT(AdbcStatementExecuteSchema(statement, schema->get(), error),
              IsOkStatus(error));
  ASSERT_THAT(AdbcStatementExecuteQuery(statement, &reader->stream.value,
                                        &reader->rows_affected, error),
              IsOkStatus(error));
  ASSERT_NO_FATAL_FAILURE(reader->GetSchema());
}

// Result columns are named as if the parameters weren't cast.
TEST_F(PostgresStatementTest, MotherDuckCastParameterColumnNames) {
  adbc_validation::StreamReader reader;
  nanoarrow::UniqueSchema schema;
  ASSERT_NO_FATAL_FAILURE(ExecuteCastQuery(
      &connection, &statement, "SELECT $1, $1 + 1, $2", "x", &reader, &schema, &error));
  for (const ArrowSchema* result : {schema.get(), &reader.schema.value}) {
    ASSERT_EQ(result->n_children, 3);
    EXPECT_STREQ(result->children[0]->name, "$1");
    EXPECT_STREQ(result->children[1]->name, "($1 + 1)");
    EXPECT_STREQ(result->children[2]->name, "$2");
  }
}

// A string is cast only if the endpoint can't type the query otherwise, since
// DuckDB won't mix a VARCHAR with, say, a DATE.
TEST_F(PostgresStatementTest, MotherDuckCastsStringParametersLast) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement,
          "CREATE OR REPLACE TABLE adbc_cast_test AS SELECT DATE '2020-01-01' AS d",
          &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  nanoarrow::UniqueSchema schema;
  ASSERT_NO_FATAL_FAILURE(ExecuteCastQuery(
      &connection, &statement,
      "SELECT $1 AS v FROM adbc_cast_test WHERE greatest(d, $2) = DATE '2020-01-05'",
      "2020-01-05", &reader, &schema, &error));
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);

  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_cast_test", &error),
              IsOkStatus(&error));
}

// If the cast query fails too, the error quotes the query as written.
TEST_F(PostgresStatementTest, MotherDuckCastRetryErrorQuotesQuery) {
  nanoarrow::UniqueSchema bind_schema;
  nanoarrow::UniqueArray bind;
  struct ArrowError na_error;
  ASSERT_THAT(
      adbc_validation::MakeSchema(
          bind_schema.get(), {{"a", NANOARROW_TYPE_INT32}, {"b", NANOARROW_TYPE_INT64}}),
      adbc_validation::IsOkErrno());
  ASSERT_THAT((adbc_validation::MakeBatch<int32_t, int64_t>(bind_schema.get(), bind.get(),
                                                            &na_error, {1}, {3})),
              adbc_validation::IsOkErrno());

  // A table function's arguments can't be parameters, cast or not.
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT $1 AS v FROM range($2)", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), bind_schema.get(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ::testing::Not(IsOkStatus(&error)));
  ASSERT_NE(error.message, nullptr);
  EXPECT_THAT(error.message,
              ::testing::HasSubstr("Query was: SELECT $1 AS v FROM range($2)"));
  EXPECT_THAT(error.message,
              ::testing::Not(::testing::HasSubstr("Query was: SELECT CAST(")));
}

// ExecuteUpdate resolves all-null parameters through the cast retry too.
TEST_F(PostgresStatementTest, MotherDuckCastsNullParametersOnUpdate) {
  nanoarrow::UniqueSchema bind_schema;
  ArrowSchemaInit(bind_schema.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(bind_schema.get(), 2),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(bind_schema->children[0], "a"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema->children[1], NANOARROW_TYPE_NA),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(bind_schema->children[1], "b"),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), bind_schema.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());
  for (int i = 0; i < 2; i++) {
    ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], i), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayAppendNull(bind->children[1], 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  }
  ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1, $2", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), bind_schema.get(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));
}

// Parameters must be bound as a struct, as for ExecuteSchema.
TEST_F(PostgresStatementTest, MotherDuckGetParameterSchemaRejectsNonStruct) {
  nanoarrow::UniqueSchema bind_schema;
  ArrowSchemaInit(bind_schema.get());
  ASSERT_THAT(ArrowSchemaSetType(bind_schema.get(), NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());
  nanoarrow::UniqueArrayStream bind;
  nanoarrow::EmptyArrayStream(bind_schema.get()).ToArrayStream(bind.get());

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBindStream(&statement, bind.get(), &error),
              IsOkStatus(&error));
  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcStatementGetParameterSchema(&statement, schema.get(), &error),
              IsStatus(ADBC_STATUS_INVALID_STATE, &error));
}

// The endpoint can't type a parameter that decides a result column's type, so
// the driver casts it to the type of its bound value (#17). Check each type it
// casts round-trips through SELECT $1.
TEST_F(PostgresStatementTest, MotherDuckCastsParameters) {
  struct Case {
    std::string name;
    std::function<ArrowErrorCode(ArrowSchema*)> set_type;
    std::function<ArrowErrorCode(ArrowArray*)> append;
    // What the bound value should equal
    std::string expected;
  };
  auto type = [](ArrowType t) {
    return [t](ArrowSchema* s) { return ArrowSchemaSetType(s, t); };
  };
  auto integer = [](int64_t v) {
    return [v](ArrowArray* a) { return ArrowArrayAppendInt(a, v); };
  };
  auto real = [](double v) {
    return [v](ArrowArray* a) { return ArrowArrayAppendDouble(a, v); };
  };
  auto string = [](std::string v) {
    return [v](ArrowArray* a) {
      return ArrowArrayAppendString(a, ArrowCharView(v.c_str()));
    };
  };
  auto timestamp = [](const char* timezone) {
    return [timezone](ArrowSchema* s) {
      return ArrowSchemaSetTypeDateTime(s, NANOARROW_TYPE_TIMESTAMP,
                                        NANOARROW_TIME_UNIT_MICRO, timezone);
    };
  };
  auto list_of = [](ArrowType t) {
    return [t](ArrowSchema* s) {
      NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(s, NANOARROW_TYPE_LIST));
      return ArrowSchemaSetType(s->children[0], t);
    };
  };

  const std::vector<Case> cases = {
      {"int16", type(NANOARROW_TYPE_INT16), integer(7), "7::SMALLINT"},
      {"int32", type(NANOARROW_TYPE_INT32), integer(-7), "-7::INTEGER"},
      {"int64", type(NANOARROW_TYPE_INT64), integer(1LL << 40), "1099511627776::BIGINT"},
      {"uint32", type(NANOARROW_TYPE_UINT32), integer(4000000000), "4000000000"},
      {"float32", type(NANOARROW_TYPE_FLOAT), real(1.5), "1.5::FLOAT"},
      {"float64", type(NANOARROW_TYPE_DOUBLE), real(-2.25), "-2.25::DOUBLE"},
      {"bool", type(NANOARROW_TYPE_BOOL), integer(1), "true"},
      {"string", type(NANOARROW_TYPE_STRING), string("h\xc3\xa9llo"), "'h\xc3\xa9llo'"},
      {"large_string", type(NANOARROW_TYPE_LARGE_STRING), string("x"), "'x'"},
      {"binary", type(NANOARROW_TYPE_BINARY),
       [](ArrowArray* a) {
         const uint8_t bytes[] = {0x01, 0xff};
         return ArrowArrayAppendBytes(a, {{bytes}, 2});
       },
       "'\\x01\\xFF'::BLOB"},
      {"date32", type(NANOARROW_TYPE_DATE32), integer(18262), "DATE '2020-01-01'"},
      {"time64",
       [](ArrowSchema* s) {
         return ArrowSchemaSetTypeDateTime(s, NANOARROW_TYPE_TIME64,
                                           NANOARROW_TIME_UNIT_MICRO, nullptr);
       },
       integer(45296789000), "TIME '12:34:56.789'"},
      {"timestamp", timestamp(nullptr), integer(1577836800123456),
       "TIMESTAMP '2020-01-01 00:00:00.123456'"},
      {"timestamptz", timestamp("UTC"), integer(1577836800123456),
       "TIMESTAMPTZ '2020-01-01 00:00:00.123456+00'"},
      {"interval", type(NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO),
       [](ArrowArray* a) {
         ArrowInterval interval;
         ArrowIntervalInit(&interval, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
         interval.months = 1;
         interval.days = 2;
         interval.ns = 3000000000;
         return ArrowArrayAppendInterval(a, &interval);
       },
       "INTERVAL '1 month 2 days 3 seconds'"},
      {"decimal128",
       [](ArrowSchema* s) {
         return ArrowSchemaSetTypeDecimal(s, NANOARROW_TYPE_DECIMAL128, 10, 5);
       },
       [](ArrowArray* a) {
         ArrowDecimal decimal;
         ArrowDecimalInit(&decimal, 128, 10, 5);
         ArrowDecimalSetInt(&decimal, 123456);
         return ArrowArrayAppendDecimal(a, &decimal);
       },
       "1.23456::DECIMAL(10, 5)"},
      {"list<int32>", list_of(NANOARROW_TYPE_INT32),
       [](ArrowArray* a) {
         NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(a->children[0], 1));
         NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(a->children[0], 2));
         return ArrowArrayFinishElement(a);
       },
       "[1, 2]"},
      {"list<string>", list_of(NANOARROW_TYPE_STRING),
       [](ArrowArray* a) {
         NANOARROW_RETURN_NOT_OK(
             ArrowArrayAppendString(a->children[0], ArrowCharView("a")));
         NANOARROW_RETURN_NOT_OK(
             ArrowArrayAppendString(a->children[0], ArrowCharView("b")));
         return ArrowArrayFinishElement(a);
       },
       "['a', 'b']"},
      // No type of its own, so it's bound as text.
      {"null", type(NANOARROW_TYPE_NA),
       [](ArrowArray* a) { return ArrowArrayAppendNull(a, 1); }, "NULL"},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    nanoarrow::UniqueSchema schema_bind;
    ArrowSchemaInit(schema_bind.get());
    ASSERT_THAT(ArrowSchemaSetTypeStruct(schema_bind.get(), 1),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(test_case.set_type(schema_bind->children[0]),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema_bind->children[0], "p"),
                adbc_validation::IsOkErrno());

    nanoarrow::UniqueArray bind;
    ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), schema_bind.get(), nullptr),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());
    ASSERT_THAT(test_case.append(bind->children[0]), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
    const std::string query =
        "SELECT $1 AS v, $1 IS NOT DISTINCT FROM " + test_case.expected + " AS eq";
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query.c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), schema_bind.get(), &error),
                IsOkStatus(&error));

    nanoarrow::UniqueSchema result_schema;
    ASSERT_THAT(AdbcStatementExecuteSchema(&statement, result_schema.get(), &error),
                IsOkStatus(&error));
    ASSERT_EQ(result_schema->n_children, 2);

    nanoarrow::UniqueSchema param_schema;
    ASSERT_THAT(AdbcStatementGetParameterSchema(&statement, param_schema.get(), &error),
                IsOkStatus(&error));
    ASSERT_EQ(param_schema->n_children, 1);

    {
      adbc_validation::StreamReader reader;
      ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                            &reader.rows_affected, &error),
                  IsOkStatus(&error));
      ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
      ASSERT_EQ(reader.schema->n_children, 2);
      EXPECT_STREQ(reader.schema->children[0]->format,
                   result_schema->children[0]->format);
      ASSERT_NO_FATAL_FAILURE(reader.Next());
      ASSERT_EQ(reader.array->length, 1);
      ASSERT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[1], 0));
      EXPECT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 0), 1);
    }
    ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));
  }
}

TEST_F(PostgresStatementTest, TransactionStatus) {
  using adbc_validation::ConnectionGetOption;
  const char* txn_status = "adbc.postgresql.transaction_status";
  ASSERT_THAT(quirks()->DropTable(&connection, "txntest", &error), IsOkStatus(&error));

  ASSERT_EQ("idle", ConnectionGetOption(&connection, txn_status, &error));

  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));

  ASSERT_EQ("idle", ConnectionGetOption(&connection, txn_status, &error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // One row finishes before ExecuteQuery returns, so the connection is already
  // idle in the transaction. More than one 1024-row chunk is still arriving.
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  const char* query = motherduck ? "SELECT i FROM range(3000) t(i)"
                                 : "SELECT g FROM generate_series(1, 3000) t(g)";

  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());

    ASSERT_EQ("active", ConnectionGetOption(&connection, txn_status, &error));

    ASSERT_THAT(AdbcConnectionRollback(&connection, &error), IsOkStatus(&error));
    ASSERT_EQ("idle", ConnectionGetOption(&connection, txn_status, &error));
  }
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());

    ASSERT_EQ("active", ConnectionGetOption(&connection, txn_status, &error));

    ASSERT_THAT(AdbcConnectionCommit(&connection, &error), IsOkStatus(&error));
    ASSERT_EQ("idle", ConnectionGetOption(&connection, txn_status, &error));
  }
}

TEST_F(PostgresStatementTest, RollbackDoesNotChainTransaction) {
  GTEST_SKIP() << "Relies on SET TRANSACTION READ ONLY, which DuckDB doesn't have";
  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SET TRANSACTION READ ONLY", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionRollback(&connection, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.use_copy",
                                     ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SHOW transaction_read_only", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());

  ASSERT_EQ(reader.array->length, 1);
  ArrowStringView view = ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
  ASSERT_EQ(std::string_view(view.data, static_cast<size_t>(view.size_bytes)), "off");
}

TEST_F(PostgresConnectionTest, GetObjectsStartsTransaction) {
  using adbc_validation::ConnectionGetOption;
  const char* txn_status = "adbc.postgresql.transaction_status";

  ASSERT_THAT(AdbcConnectionNew(&connection, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&connection, &database, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
  ASSERT_EQ("idle", ConnectionGetOption(&connection, txn_status, &error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcConnectionGetObjects(&connection, ADBC_OBJECT_DEPTH_CATALOGS, nullptr, nullptr,
                               nullptr, nullptr, nullptr, &reader.stream.value, &error),
      IsOkStatus(&error));
  ASSERT_EQ("intrans", ConnectionGetOption(&connection, txn_status, &error));
}

TEST_F(PostgresStatementTest, IsolationLevels) {
  GTEST_SKIP()
      << "DuckDB has a single isolation level; the endpoint rejects SET TRANSACTION";
  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_ISOLATION_LEVEL,
                                      ADBC_OPTION_ISOLATION_LEVEL_SERIALIZABLE, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT current_setting('transaction_isolation')", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());

  ASSERT_EQ(reader.array->length, 1);
  ArrowStringView view = ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
  std::string_view isolation_level(view.data, static_cast<size_t>(view.size_bytes));
  ASSERT_EQ(isolation_level, "serializable");
}

TEST_F(PostgresStatementTest, SqlIngestSchema) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  const std::string schema_name = TestSchema("testschema");
  const std::string quoted_schema = quirks()->QuoteIdentifier(schema_name);

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, ("CREATE SCHEMA IF NOT EXISTS " + quoted_schema).c_str(), &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  std::string drop = "DROP TABLE IF EXISTS " + quoted_schema + ".schematable";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, drop.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "ints"),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {-1, 0, 1, std::nullopt})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "schematable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_CREATE, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                                       schema_name.c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }
}

TEST_F(PostgresStatementTest, SqlIngestReplaceRecreatesTable) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "CREATE OR REPLACE TABLE ingest_replace_ddl "
                                       "(old_col INTEGER)",
                                       &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "ints"),
                adbc_validation::IsOkErrno());
    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {1, 2})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "ingest_replace_ddl", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_REPLACE, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    // MotherDuck rejects COPY FROM STDIN (#31), so only the DDL before it is
    // checked here: the drop and create must succeed even if the upload fails.
    if (AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error) !=
        ADBC_STATUS_OK) {
      ASSERT_NE(error.message, nullptr);
      EXPECT_THAT(error.message, ::testing::Not(::testing::HasSubstr("Failed to drop")));
      EXPECT_THAT(error.message,
                  ::testing::Not(::testing::HasSubstr("Failed to create")));
      error.release(&error);
    }
  }

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT * FROM ingest_replace_ddl", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(reader.schema->n_children, 1);
  EXPECT_STREQ(reader.schema->children[0]->name, "ints");
}

TEST_F(PostgresStatementTest, SqlIngestTemporaryTable) {
  GTEST_SKIP() << "The Postgres endpoint rejects CREATE TEMP TABLE";
  ASSERT_THAT(quirks()->DropTempTable(&connection, "temptable", &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "CREATE TEMPORARY TABLE temptable (ints BIGINT)", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionCommit(&connection, &error), IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "ints"),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {-1, 0, 1, std::nullopt})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "temptable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_APPEND, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    // because temporary table
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  }

  ASSERT_THAT(AdbcConnectionRollback(&connection, &error), IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "ints"),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {-1, 0, 1, std::nullopt})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "temptable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_APPEND, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TEMPORARY,
                                       ADBC_OPTION_VALUE_ENABLED, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }
}

// Appending looks up the target's column types before COPY, so these errors
// surface even without COPY FROM STDIN (#13).
TEST_F(PostgresStatementTest, SqlIngestAppendTargetLookupErrors) {
  ASSERT_THAT(quirks()->DropTable(&connection, "no_such_ingest_table", &error),
              IsOkStatus(&error));
  ASSERT_THAT(quirks()->DropTable(&connection, "ingest_lookup", &error),
              IsOkStatus(&error));
  {
    adbc_validation::Handle<struct AdbcStatement> create;
    ASSERT_THAT(AdbcStatementNew(&connection, &create.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &create.value, "CREATE TABLE ingest_lookup (amount BIGINT)", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&create.value, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }
  // Drop the table however the test exits, including through a failed ASSERT.
  struct OnExit {
    std::function<void()> run;
    ~OnExit() { run(); }
  } drop_on_exit{[this]() {
    struct AdbcError drop_error = {};
    EXPECT_THAT(quirks()->DropTable(&connection, "ingest_lookup", &drop_error),
                IsOkStatus(&drop_error));
  }};

  auto append = [&](const char* db_schema, const char* target,
                    const char* column) -> AdbcStatusCode {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;
    ArrowSchemaInit(&schema.value);
    EXPECT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    EXPECT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
                adbc_validation::IsOkErrno());
    EXPECT_THAT(ArrowSchemaSetName(schema->children[0], column),
                adbc_validation::IsOkErrno());
    EXPECT_THAT(
        (adbc_validation::MakeBatch<int64_t>(
            &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr), {1})),
        adbc_validation::IsOkErrno());

    adbc_validation::Handle<struct AdbcStatement> stmt;
    EXPECT_THAT(AdbcStatementNew(&connection, &stmt.value, &error), IsOkStatus(&error));
    EXPECT_THAT(AdbcStatementSetOption(&stmt.value, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       target, &error),
                IsOkStatus(&error));
    if (db_schema != nullptr) {
      EXPECT_THAT(AdbcStatementSetOption(&stmt.value, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                                         db_schema, &error),
                  IsOkStatus(&error));
    }
    EXPECT_THAT(AdbcStatementSetOption(&stmt.value, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_APPEND, &error),
                IsOkStatus(&error));
    EXPECT_THAT(AdbcStatementBind(&stmt.value, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    return AdbcStatementExecuteQuery(&stmt.value, nullptr, nullptr, &error);
  };

  ASSERT_THAT(append(nullptr, "no_such_ingest_table", "amount"),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_THAT(append("no_such_ingest_schema", "ingest_lookup", "amount"),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_THAT(append(nullptr, "ingest_lookup", "not_a_column"),
              IsStatus(ADBC_STATUS_ALREADY_EXISTS, &error));

  // DuckDB column names ignore case, so a differently cased column passes the
  // lookup; if the append still fails, it's at COPY FROM STDIN, which the
  // endpoint lacks (#31).
  if (append(nullptr, "INGEST_LOOKUP", "AMOUNT") != ADBC_STATUS_OK) {
    EXPECT_THAT(error.message ? error.message : "",
                ::testing::HasSubstr("COPY query failed"));
  }
  if (error.release) error.release(&error);
}

TEST_F(PostgresStatementTest, SqlIngestAppendIntegerIntoNumeric) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(quirks()->DropTable(&connection, "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "CREATE TABLE numeric_ingest (amount NUMERIC(20, 2))", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;

  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "amount"),
              adbc_validation::IsOkErrno());

  ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                  &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                  {1, 42, 9999, -7, std::nullopt})),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_APPEND, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT amount::text FROM numeric_ingest "
                                       "ORDER BY numeric_ingest.amount NULLS FIRST",
                                       &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
      reader.array_view->children[0],
      {std::nullopt, "-7.00", "1.00", "42.00", "9999.00"}));
}

TEST_F(PostgresStatementTest, SqlIngestAppendIntegerListIntoNumericArray) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(quirks()->DropTable(&connection, "numeric_array_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement,
                  "CREATE TABLE numeric_array_ingest (amounts NUMERIC(20, 2)[])", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  struct ArrowError na_error;

  ASSERT_EQ(adbc_validation::MakeSchema(
                &schema.value,
                {adbc_validation::SchemaField::Nested("amounts", NANOARROW_TYPE_LIST,
                                                      {{"item", NANOARROW_TYPE_INT64}})}),
            ADBC_STATUS_OK);
  ASSERT_EQ(
      adbc_validation::MakeBatch<std::vector<int64_t>>(
          &schema.value, &batch.value, &na_error,
          {std::vector<int64_t>{1, 42}, std::vector<int64_t>{-7, 9999}, std::nullopt}),
      ADBC_STATUS_OK);

  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "numeric_array_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_APPEND, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement,
                  "SELECT amounts::text FROM numeric_array_ingest ORDER BY ctid", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
      reader.array_view->children[0], {"{1.00,42.00}", "{-7.00,9999.00}", std::nullopt}));
}

TEST_F(PostgresStatementTest, SqlIngestAppendIntegerBoundsIntoNumeric) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(quirks()->DropTable(&connection, "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "CREATE TABLE numeric_ingest ("
                                       "signed_amount NUMERIC(30, 0), "
                                       "unsigned_amount NUMERIC(30, 0))",
                                       &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;

  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 2), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "signed_amount"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_UINT64),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[1], "unsigned_amount"),
              adbc_validation::IsOkErrno());

  ASSERT_THAT((adbc_validation::MakeBatch<int64_t, uint64_t>(
                  &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                  {std::numeric_limits<int64_t>::min(), -10000, 0, 10000,
                   std::numeric_limits<int64_t>::max()},
                  {0, 10000, std::numeric_limits<uint64_t>::max(), 42, 1})),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_APPEND, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement,
                               "SELECT signed_amount::text, unsigned_amount::text "
                               "FROM numeric_ingest ORDER BY ctid",
                               &error),
      IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
      reader.array_view->children[0],
      {"-9223372036854775808", "-10000", "0", "10000", "9223372036854775807"}));
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
      reader.array_view->children[1], {"0", "10000", "18446744073709551615", "42", "1"}));
}

TEST_F(PostgresStatementTest, SqlIngestAppendFloatIntoNumericNotImplemented) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(quirks()->DropTable(&connection, "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "CREATE TABLE numeric_ingest (amount NUMERIC(20, 2))", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;

  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_DOUBLE),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "amount"),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(
      (adbc_validation::MakeBatch<double>(
          &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr), {1.25})),
      adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "numeric_ingest", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_APPEND, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_INTERNAL, &error));
  ASSERT_THAT(error.message,
              ::testing::HasSubstr(
                  "COPY Writer from Arrow type 'double' to PostgreSQL numeric is not "
                  "implemented"));
}

TEST_F(PostgresStatementTest, SqlIngestTimestampOverflow) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "$1"),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_SECOND, nullptr),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {std::numeric_limits<int64_t>::max()})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement, "SELECT CAST($1 AS TIMESTAMP)", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementPrepare(&statement, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsStatus(ADBC_STATUS_INTERNAL, &error));
    ASSERT_THAT(
        error.message,
        ::testing::HasSubstr(
            "Row 0 timestamp value 9223372036854775807 with unit 0 would overflow"));
  }

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "$1"),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_SECOND, nullptr),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {std::numeric_limits<int64_t>::min()})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement, "SELECT CAST($1 AS TIMESTAMP)", &error),
        IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementPrepare(&statement, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsStatus(ADBC_STATUS_INTERNAL, &error));
    ASSERT_THAT(
        error.message,
        ::testing::HasSubstr(
            "Row 0 timestamp value -9223372036854775808 with unit 0 would overflow"));
  }
}

TEST_F(PostgresStatementTest, SqlIngestJson) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  std::string drop = "DROP TABLE IF EXISTS jsontable";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, drop.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_STRING),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "j"),
                adbc_validation::IsOkErrno());

    nanoarrow::UniqueBuffer buffer;
    ASSERT_THAT(ArrowMetadataBuilderInit(buffer.get(), nullptr),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(
        ArrowMetadataBuilderAppend(buffer.get(), ArrowCharView("ARROW:extension:name"),
                                   ArrowCharView("arrow.json")),
        adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetMetadata(schema->children[0],
                                       reinterpret_cast<char*>(buffer->data)),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<std::string>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {R"({"a": 1, "b": [1, 2, 3]})", std::nullopt})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "jsontable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_CREATE, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  // Check round-trip
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT * FROM jsontable", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));

  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(1, reader.fields.size());
  ASSERT_EQ(NANOARROW_TYPE_STRING, reader.fields[0].type);
  ASSERT_EQ(
      "arrow.json",
      std::string_view(reader.fields[0].extension_name.data,
                       static_cast<size_t>(reader.fields[0].extension_name.size_bytes)));

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ArrowStringView view = ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
  std::string_view v(view.data, static_cast<size_t>(view.size_bytes));
  ASSERT_EQ(R"({"a": 1, "b": [1, 2, 3]})", v);
}

// Ensure the table is actually created with the JSON type by trying to ingest
// invalid JSON
TEST_F(PostgresStatementTest, SqlIngestJsonInvalid) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  std::string drop = "DROP TABLE IF EXISTS jsontable";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, drop.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;

  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_STRING),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "j"), adbc_validation::IsOkErrno());

  nanoarrow::UniqueBuffer buffer;
  ASSERT_THAT(ArrowMetadataBuilderInit(buffer.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(
      ArrowMetadataBuilderAppend(buffer.get(), ArrowCharView("ARROW:extension:name"),
                                 ArrowCharView("arrow.json")),
      adbc_validation::IsOkErrno());
  ASSERT_THAT(
      ArrowSchemaSetMetadata(schema->children[0], reinterpret_cast<char*>(buffer->data)),
      adbc_validation::IsOkErrno());

  ASSERT_THAT((adbc_validation::MakeBatch<std::string>(
                  &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                  {R"({)", std::nullopt})),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "jsontable", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_CREATE, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_INVALID_DATA, &error));
  ASSERT_THAT(error.message, ::testing::HasSubstr("invalid input syntax for type json"));
}

TEST_F(PostgresStatementTest, SqlIngestJsonb) {
  GTEST_SKIP() << "JSONB is PostgreSQL-only";
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  std::string drop = "DROP TABLE IF EXISTS jsontable";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, drop.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  drop = "CREATE TABLE jsontable (j JSONB)";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, drop.c_str(), &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  {
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;

    ArrowSchemaInit(&schema.value);
    ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_STRING),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "j"),
                adbc_validation::IsOkErrno());

    nanoarrow::UniqueBuffer buffer;
    ASSERT_THAT(ArrowMetadataBuilderInit(buffer.get(), nullptr),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(
        ArrowMetadataBuilderAppend(buffer.get(), ArrowCharView("ARROW:extension:name"),
                                   ArrowCharView("arrow.json")),
        adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetMetadata(schema->children[0],
                                       reinterpret_cast<char*>(buffer->data)),
                adbc_validation::IsOkErrno());

    ASSERT_THAT((adbc_validation::MakeBatch<std::string>(
                    &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                    {R"({"a": 1, "b": [1, 2, 3]})", std::nullopt})),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "jsontable", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                                       ADBC_INGEST_OPTION_MODE_APPEND, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));

    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "SELECT j::text FROM jsontable ORDER BY ctid", &error),
                IsOkStatus(&error));

    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);
    ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
        reader.array_view->children[0], {R"({"a": 1, "b": [1, 2, 3]})", std::nullopt}));
  }
}

TEST_F(PostgresStatementTest, SqlReadIntervalOverflow) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "SELECT CAST('2562048 hours' AS INTERVAL)", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    // A streamed result still has rows, so the count is unknown.
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_THAT(reader.MaybeNext(),
                adbc_validation::IsErrno(EINVAL, &reader.stream.value, nullptr));
    ASSERT_THAT(reader.stream->get_last_error(&reader.stream.value),
                ::testing::HasSubstr("Interval with time value 9223372800000000 usec "
                                     "would overflow when converting to nanoseconds"));
    AdbcStatusCode status = ADBC_STATUS_OK;
    const struct AdbcError* detail =
        AdbcErrorFromArrayStream(&reader.stream.value, &status);
    ASSERT_NE(nullptr, detail);
    ASSERT_EQ(ADBC_STATUS_INTERNAL, status);
    ASSERT_THAT(detail->message,
                ::testing::HasSubstr("would overflow when converting to nanoseconds"));
    ASSERT_EQ(reader.array->release, nullptr);
  }

  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "SELECT CAST('-2562048 hours' AS INTERVAL)", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_THAT(reader.MaybeNext(),
                adbc_validation::IsErrno(EINVAL, &reader.stream.value, nullptr));
    ASSERT_THAT(reader.stream->get_last_error(&reader.stream.value),
                ::testing::HasSubstr("Interval with time value -9223372800000000 usec "
                                     "would overflow when converting to nanoseconds"));
    AdbcStatusCode status = ADBC_STATUS_OK;
    const struct AdbcError* detail =
        AdbcErrorFromArrayStream(&reader.stream.value, &status);
    ASSERT_NE(nullptr, detail);
    ASSERT_EQ(ADBC_STATUS_INTERNAL, status);
    ASSERT_THAT(detail->message,
                ::testing::HasSubstr("would overflow when converting to nanoseconds"));
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

TEST_F(PostgresStatementTest, ScriptReadErrorRollsBack) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  for (const char* query : {"DROP TABLE IF EXISTS test_stream_read_error",
                            "CREATE TABLE test_stream_read_error (ints INT)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "INSERT INTO test_stream_read_error VALUES (1); "
                                       "SELECT CAST('2562048 hours' AS INTERVAL)",
                                       &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_THAT(reader.MaybeNext(),
                adbc_validation::IsErrno(EINVAL, &reader.stream.value, nullptr));
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT COUNT(*) FROM test_stream_read_error", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader count;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &count.stream.value, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(count.GetSchema());
  ASSERT_NO_FATAL_FAILURE(count.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(count.array_view->children[0], 0), 0);
}

TEST_F(PostgresStatementTest, UpdateInExecuteQuery) {
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_test", &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement,
                    "CREATE TABLE adbc_test (ints INT, id INTEGER PRIMARY KEY)", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  {
    // Use INSERT INTO
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "INSERT INTO adbc_test (ints, id) VALUES (1, 1), (2, 2)", &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, 2);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  {
    // Use INSERT INTO ... RETURNING
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement,
                    "INSERT INTO adbc_test (ints, id) VALUES (3, 3), (4, 4) RETURNING id",
                    &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    // The RETURNING rows are still unread, so the count is unknown.
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(reader.array->release, nullptr);
    ASSERT_EQ(reader.array->n_children, 1);
    ASSERT_EQ(reader.array->length, 2);
    ASSERT_EQ(reader.array_view->children[0]->buffer_views[1].data.as_int32[0], 3);
    ASSERT_EQ(reader.array_view->children[0]->buffer_views[1].data.as_int32[1], 4);
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

TEST_F(PostgresStatementTest, ExecuteSchemaParameterizedQuery) {
  nanoarrow::UniqueSchema schema_bind;
  ArrowSchemaInit(schema_bind.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(schema_bind.get(), 1),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema_bind->children[0], NANOARROW_TYPE_STRING),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArrayStream bind;
  nanoarrow::EmptyArrayStream(schema_bind.get()).ToArrayStream(bind.get());

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBindStream(&statement, bind.get(), &error), IsOkStatus());

  nanoarrow::UniqueSchema schema;
  ASSERT_THAT(AdbcStatementExecuteSchema(&statement, schema.get(), &error),
              IsOkStatus(&error));

  ASSERT_EQ(1, schema->n_children);
  ASSERT_STREQ("u", schema->children[0]->format);

  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));
}

TEST_F(PostgresStatementTest, NumericTypeModifier) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  // MotherDuck rejects binary COPY TO STDOUT (#11), so only the non-COPY result
  // path is exercised. The endpoint reports typmod -1 for every numeric column,
  // and a bare numeric is DuckDB's DECIMAL(18,3), so 1.25 reads back as 1.250.
  ASSERT_THAT(
      AdbcStatementSetOption(&statement, "adbc.postgresql.use_copy", "false", &error),
      IsOkStatus(&error));
  for (const char* predicate : {"true", "false"}) {
    SCOPED_TRACE(predicate);
    const std::string query =
        "SELECT 1.25::numeric(29,9), 1.25::numeric, NULL::numeric(29,9), "
        "1.25::numeric(29,9) + 1.25::numeric(29,9), 1::integer WHERE "s +
        predicate;
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query.c_str(), &error),
                IsOkStatus(&error));
    nanoarrow::UniqueSchema described;
    ASSERT_THAT(AdbcStatementExecuteSchema(&statement, described.get(), &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    for (const ArrowSchema* schema : {described.get(), &reader.schema.value}) {
      ASSERT_EQ(schema->n_children, 5);
      for (int i = 0; i < 5; i++) {
        ArrowStringView value = {nullptr, 0};
        ASSERT_EQ(ArrowMetadataGetValue(schema->children[i]->metadata,
                                        ArrowCharView("POSTGRESQL:typmod"), &value),
                  NANOARROW_OK);
        if (i == 4) {
          EXPECT_EQ(value.data, nullptr);
        } else {
          EXPECT_STREQ(schema->children[i]->format, "u");
          EXPECT_EQ(std::string(value.data, value.size_bytes), "-1");
        }
      }
    }
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    if (std::string(predicate) == "true") {
      ASSERT_NE(reader.array->release, nullptr);
      ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
          reader.array_view->children[0], {"1.250000000"}));
      ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
          reader.array_view->children[1], {"1.250"}));
      ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
          reader.array_view->children[2], {std::nullopt}));
      ASSERT_NO_FATAL_FAILURE(reader.Next());
    }
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

TEST_F(PostgresStatementTest, ExecuteParameterizedQueryWithResult) {
  nanoarrow::UniqueSchema schema_bind;
  ArrowSchemaInit(schema_bind.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(schema_bind.get(), 1),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema_bind->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), schema_bind.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], 123), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], 456), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendNull(bind->children[0], 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT $1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), schema_bind.get(), &error),
              IsOkStatus());

  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_EQ(reader.schema->n_children, 1);

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(reader.array_view->children[0]->buffer_views[1].data.as_int32[0], 123);

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(reader.array_view->children[0]->buffer_views[1].data.as_int32[0], 456);

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(reader.array->children[0]->null_count, 1);

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

TEST_F(PostgresStatementTest, ExecuteParameterizedQueryWithRowsAffected) {
  // Check that when executing one or more parameterized queries that the corresponding
  // affected row count is added.
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_test", &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement, "CREATE TABLE adbc_test (ints INT)", &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, -1);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  {
    // Use INSERT INTO
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "INSERT INTO adbc_test (ints) VALUES (123), (456)", &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(reader.rows_affected, 2);
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  nanoarrow::UniqueSchema schema_bind;
  ArrowSchemaInit(schema_bind.get());
  ASSERT_THAT(ArrowSchemaSetTypeStruct(schema_bind.get(), 1),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema_bind->children[0], NANOARROW_TYPE_INT32),
              adbc_validation::IsOkErrno());

  nanoarrow::UniqueArray bind;
  ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), schema_bind.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], 123), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayAppendInt(bind->children[0], 456), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "DELETE FROM adbc_test WHERE ints = $1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), schema_bind.get(), &error),
              IsOkStatus());

  {
    int64_t rows_affected = -2;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, &rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_EQ(rows_affected, 2);
  }

  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT * from adbc_test", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

// Test for making sure empty string/binary parameters are inserted correct
TEST_F(PostgresStatementTest, EmptyStringAndBinaryParameter) {
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_test", &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // Create test table with both TEXT and BYTEA columns
  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement,
                    "CREATE TABLE adbc_test (text_data TEXT, binary_data BYTEA)", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  // Insert empty string and binary via parameters
  {
    nanoarrow::UniqueSchema schema_bind;
    ArrowSchemaInit(schema_bind.get());
    ASSERT_THAT(ArrowSchemaSetTypeStruct(schema_bind.get(), 2),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema_bind->children[0], NANOARROW_TYPE_STRING),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowSchemaSetType(schema_bind->children[1], NANOARROW_TYPE_BINARY),
                adbc_validation::IsOkErrno());

    nanoarrow::UniqueArray bind;
    ASSERT_THAT(ArrowArrayInitFromSchema(bind.get(), schema_bind.get(), nullptr),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayStartAppending(bind.get()), adbc_validation::IsOkErrno());

    // Add one row with empty string and empty binary parameters
    ASSERT_THAT(ArrowArrayAppendString(bind->children[0], ArrowCharView("")),
                adbc_validation::IsOkErrno());
    ArrowBufferView empty_buffer = {{nullptr}, 0};
    ASSERT_THAT(ArrowArrayAppendBytes(bind->children[1], empty_buffer),
                adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayFinishElement(bind.get()), adbc_validation::IsOkErrno());
    ASSERT_THAT(ArrowArrayFinishBuildingDefault(bind.get(), nullptr),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                         "INSERT INTO adbc_test VALUES ($1, $2)", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, bind.get(), schema_bind.get(), &error),
                IsOkStatus(&error));

    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  // Verify empty values were inserted correctly (not as NULL)
  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "SELECT text_data, binary_data FROM adbc_test", &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(reader.array->release, nullptr);
    ASSERT_EQ(reader.array->length, 1);

    // Row should contain empty values, not NULL
    ASSERT_EQ(reader.array->children[0]->null_count, 0);  // text_data
    ASSERT_EQ(reader.array->children[1]->null_count, 0);  // binary_data

    // Check that both values are empty (string and binary)
    // Check the single row
    ASSERT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[0], 0));
    struct ArrowBufferView string_view =
        ArrowArrayViewGetBytesUnsafe(reader.array_view->children[0], 0);
    ASSERT_EQ(string_view.size_bytes, 0);  // Empty string should have size 0

    ASSERT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[1], 0));
    struct ArrowBufferView binary_view =
        ArrowArrayViewGetBytesUnsafe(reader.array_view->children[1], 0);
    ASSERT_EQ(binary_view.size_bytes, 0);  // Empty binary should have size 0

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }

  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));
}

// Regression test for https://github.com/apache/arrow-adbc/issues/4319.
// The parameterized prepared-statement path (BindAndExecuteCurrentRow)
// constructs its field writers via the same MakeCopyFieldWriter factory
// as the COPY ingest path, so the offset bug in PostgresCopyListFieldWriter
// drifts list-typed bound parameters the same way. This exercises the bind
// path end-to-end with an INSERT … ON CONFLICT DO UPDATE upsert whose list
// parameter comes from a sliced Arrow array (parent.offset > 0).
TEST_F(PostgresStatementTest, BindUpsertWithSlicedListParameter) {
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_test", &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "CREATE TABLE adbc_test (id INT PRIMARY KEY, tags TEXT[] NOT NULL)",
          &error),
      IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
  }

  // 6-row struct(int32, list<string>) where odd rows have 1-element tags
  // and even rows have 2-element tags. The size difference makes drift
  // structurally observable, not just value-different.
  adbc_validation::Handle<struct ArrowSchema> bind_schema;
  adbc_validation::Handle<struct ArrowArray> bind;
  struct ArrowError na_error;
  ASSERT_EQ(adbc_validation::MakeSchema(
                &bind_schema.value,
                {{"id", NANOARROW_TYPE_INT32},
                 adbc_validation::SchemaField::Nested(
                     "tags", NANOARROW_TYPE_LIST, {{"item", NANOARROW_TYPE_STRING}})}),
            ADBC_STATUS_OK);

  ASSERT_EQ(
      (adbc_validation::MakeBatch<int32_t, std::vector<std::string>>(
          &bind_schema.value, &bind.value, &na_error, {0, 1, 2, 3, 4, 5},
          {std::vector<std::string>{"r0a", "r0b"}, std::vector<std::string>{"r1x"},
           std::vector<std::string>{"r2a", "r2b"}, std::vector<std::string>{"r3x"},
           std::vector<std::string>{"r4a", "r4b"}, std::vector<std::string>{"r5x"}})),
      ADBC_STATUS_OK);

  // Hide rows 0..2 by setting offset/length on the struct root and on
  // each child column
  bind->offset = 0;
  bind->length = 3;
  bind->children[0]->offset = 3;
  bind->children[0]->length = 3;
  bind->children[1]->offset = 3;
  bind->children[1]->length = 3;

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement,
                               "INSERT INTO adbc_test (id, tags) VALUES ($1, $2) "
                               "ON CONFLICT (id) DO UPDATE SET tags = EXCLUDED.tags",
                               &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &bind.value, &bind_schema.value, &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
        IsOkStatus(&error));
  }

  // SELECT array_length(tags, 1) per id. Expected: id=3→1, id=4→2, id=5→1.
  // Under the bug, ids 3..5 would receive tags from underlying rows 0..2
  // and report lengths 2/1/2 instead.
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement,
          "SELECT id, array_length(tags, 1) AS len FROM adbc_test ORDER BY id", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(reader.array->release, nullptr);
  ASSERT_EQ(reader.array->length, 3);

  // id column (int32)
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 3);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 1), 4);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 2), 5);
  // len column (int32 — PostgreSQL array_length returns int4)
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 0), 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 1), 2);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[1], 2), 1);

  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));
}

TEST_F(PostgresStatementTest, SqlExecuteCopyZeroRowOutputError) {
  GTEST_SKIP() << "Uses JSONB and the COPY result path, both PostgreSQL-only";
  ASSERT_THAT(quirks()->DropTable(&connection, "adbc_test", &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  {
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "CREATE TABLE adbc_test (id int primary key, data jsonb)",
                    &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
  }

  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "insert into adbc_test (id, data) values (1, null)", &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
  }

  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "insert into adbc_test (id, data) values (2, '1')", &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
  }

  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(&statement,
                                 "SELECT id, data from adbc_test JOIN "
                                 "jsonb_array_elements(adbc_test.data) AS foo ON true",
                                 &error),
        IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus());
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_EQ(reader.MaybeNext(), EIO);

    AdbcStatusCode status = ADBC_STATUS_OK;
    const struct AdbcError* detail =
        AdbcErrorFromArrayStream(&reader.stream.value, &status);
    ASSERT_NE(nullptr, detail);
    ASSERT_EQ(ADBC_STATUS_INVALID_DATA, status);
    ASSERT_EQ("22023", std::string_view(detail->sqlstate, 5));
  }
}

TEST_F(PostgresStatementTest, BatchSizeHint) {
  SKIP_WITHOUT_COPY_FROM_STDIN();
  ASSERT_THAT(quirks()->EnsureSampleTable(&connection, "batch_size_hint_test", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // Setting the batch size hint to a negative or non-integer value should fail
  ASSERT_EQ(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                   "-1", nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);
  ASSERT_EQ(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                   "not a valid number", nullptr),
            ADBC_STATUS_INVALID_ARGUMENT);

  // For this test, use a batch size of 1 byte to force every row to be its own batch
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));

  {
    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement, "SELECT int64s from batch_size_hint_test ORDER BY int64s LIMIT 3",
            &error),
        IsOkStatus(&error));

    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), -42);
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 42);
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_TRUE(ArrowArrayViewIsNull(reader.array_view->children[0], 0));
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->release, nullptr);
  }
}

// Test that an ADBC 1.0.0-sized error still works
TEST_F(PostgresStatementTest, AdbcErrorBackwardsCompatibility) {
  struct AdbcError error;
  std::memset(&error, 0, ADBC_ERROR_1_1_0_SIZE);
  struct AdbcDriver canary;
  error.private_data = &canary;
  error.private_driver = &canary;

  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT * FROM thistabledoesnotexist", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_EQ("42P01", std::string_view(error.sqlstate, 5));
  ASSERT_EQ(0, AdbcErrorGetDetailCount(&error));
  ASSERT_EQ(&canary, error.private_data);
  ASSERT_EQ(&canary, error.private_driver);
  error.release(&error);
}

TEST_F(PostgresStatementTest, ResultStreamBatchSizeHint) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  for (const char* query :
       {"DROP TABLE IF EXISTS test_batch_hint", "CREATE TABLE test_batch_hint (ints INT)",
        "INSERT INTO test_batch_hint (ints) VALUES (1), (2), (3)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT ints FROM test_batch_hint ORDER BY ints", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  // The count is not known at ExecuteQuery, and it is not written later.
  ASSERT_EQ(reader.rows_affected, -1);
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
  ASSERT_EQ(reader.rows_affected, -1);
  for (int32_t expected = 2; expected <= 3; expected++) {
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
    ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), expected);
  }
  ASSERT_EQ(reader.rows_affected, -1);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->release, nullptr);
  ASSERT_EQ(reader.rows_affected, -1);
}

TEST_F(PostgresStatementTest, Cancel) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // MotherDuck does not send a row until every sleep in the query has
  // finished, so Cancel has to arrive while ExecuteQuery is still blocked.
  // sleep_ms is MotherDuck; PostgreSQL has pg_sleep, in seconds.
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, motherduck ? "SELECT sleep_ms(5000)" : "SELECT pg_sleep(5)",
                  &error),
              IsOkStatus(&error));

  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    struct AdbcError cancel_error = ADBC_ERROR_INIT;
    AdbcStatementCancel(&statement, &cancel_error);
    if (cancel_error.release != nullptr) cancel_error.release(&cancel_error);
  });

  adbc_validation::StreamReader reader;
  const AdbcStatusCode status = AdbcStatementExecuteQuery(
      &statement, &reader.stream.value, &reader.rows_affected, &error);
  canceller.join();
  ASSERT_THAT(status, IsStatus(ADBC_STATUS_CANCELLED, &error));
  ASSERT_EQ("57014", std::string_view(error.sqlstate, 5));
  if (error.release != nullptr) error.release(&error);
}

TEST_F(PostgresStatementTest, MultipleStatementsSingleQuery) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  const char* query = R"(DROP TABLE IF EXISTS test_query_statements;
            CREATE TABLE test_query_statements (ints INT);
            INSERT INTO test_query_statements VALUES((1));
            INSERT INTO test_query_statements VALUES((2));
            INSERT INTO test_query_statements VALUES((3));)";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT * FROM test_query_statements", &error),
      IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  reader.GetSchema();
  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->length, 3);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.rows_affected, -1);
}

TEST_F(PostgresStatementTest, MultipleStatementsResultStream) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // The last statement is the result. The earlier one still has to run.
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT 1 AS n; SELECT 2 AS n", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 2);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.rows_affected, -1);
}

TEST_F(PostgresStatementTest, MultipleStatementsResultStreamError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // The failing statement is reported. The statement after it does not run.
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT * FROM test_stream_missing; SELECT 2 AS n", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_EQ("42P01", std::string_view(error.sqlstate, 5));
  error.release(&error);
}

TEST_F(PostgresStatementTest, MultipleStatementsResultStreamQuotedSemicolon) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // The semicolon is inside a literal, so it is not a statement boundary.
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT 'a;b' AS s; SELECT 2 AS n", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 2);
}

TEST_F(PostgresStatementTest, ResultStreamReplacedWhileOpen) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  for (const char* query :
       {"DROP TABLE IF EXISTS test_stream_replace",
        "CREATE TABLE test_stream_replace (ints INT)",
        "INSERT INTO test_stream_replace (ints) VALUES (1), (2), (3)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "SELECT ints FROM test_stream_replace ORDER BY ints", &error),
      IsOkStatus(&error));

  adbc_validation::StreamReader first;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &first.stream.value,
                                        &first.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(first.GetSchema());
  ASSERT_NO_FATAL_FAILURE(first.Next());
  ASSERT_EQ(first.array->length, 1);
  ASSERT_EQ(first.rows_affected, -1);

  // The result is already buffered, so the next command must not invalidate it.
  adbc_validation::StreamReader second;
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 7 AS n", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &second.stream.value,
                                        &second.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(second.GetSchema());
  ASSERT_NO_FATAL_FAILURE(second.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(second.array_view->children[0], 0), 7);

  ASSERT_NO_FATAL_FAILURE(first.Next());
  ASSERT_EQ(first.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(first.array_view->children[0], 0), 2);
}

TEST_F(PostgresStatementTest, ResultStreamOutlivesStatement) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT g FROM generate_series(1, 3) t(g)", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());

  // Releasing the statement must not free the connection out from under the
  // stream. The reader keeps the connection alive and the remaining rows
  // can still be read.
  ASSERT_THAT(AdbcStatementRelease(&statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 2);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 3);
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->release, nullptr);
}

TEST_F(PostgresStatementTest, ResultStreamReleasedBeforeNextQuery) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));
  {
    // The stream owns the reader. Dropping it finishes the query, so the next
    // ExecuteQuery on this statement can use the connection.
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement, "SELECT g FROM generate_series(1, 3) t(g)", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);
  }

  adbc_validation::StreamReader next;
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 4 AS n", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &next.stream.value, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(next.GetSchema());
  ASSERT_NO_FATAL_FAILURE(next.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(next.array_view->children[0], 0), 4);
}

TEST_F(PostgresStatementTest, CancelBetweenReads) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.batch_size_hint_bytes",
                                     "1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT g FROM generate_series(1, 3) t(g)", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);

  ASSERT_THAT(AdbcStatementCancel(&statement, &error), IsOkStatus(&error));
  ASSERT_EQ(ECANCELED, reader.MaybeNext());

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 9", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader next;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &next.stream.value, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(next.GetSchema());
  ASSERT_NO_FATAL_FAILURE(next.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(next.array_view->children[0], 0), 9);
}

TEST_F(PostgresStatementTest, CancelNullStreamIgnoresOldReader) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));

  // The buffered stream must not stay the cancel target. This null-stream
  // query has to be the one that gets cancelled.
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, motherduck ? "SELECT sleep_ms(5000)" : "SELECT pg_sleep(5)",
                  &error),
              IsOkStatus(&error));
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    struct AdbcError cancel_error = ADBC_ERROR_INIT;
    AdbcStatementCancel(&statement, &cancel_error);
    if (cancel_error.release != nullptr) cancel_error.release(&cancel_error);
  });
  const AdbcStatusCode status =
      AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error);
  canceller.join();
  ASSERT_THAT(status, IsStatus(ADBC_STATUS_CANCELLED, &error));
  if (error.release != nullptr) error.release(&error);

  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, CancelLeadingStatement) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       motherduck ? "SELECT sleep_ms(5000); SELECT 1"
                                                  : "SELECT pg_sleep(5); SELECT 1",
                                       &error),
              IsOkStatus(&error));
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    struct AdbcError cancel_error = ADBC_ERROR_INIT;
    AdbcStatementCancel(&statement, &cancel_error);
    if (cancel_error.release != nullptr) cancel_error.release(&cancel_error);
  });
  adbc_validation::StreamReader reader;
  const AdbcStatusCode status =
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error);
  canceller.join();
  ASSERT_THAT(status, IsStatus(ADBC_STATUS_CANCELLED, &error));
  if (error.release != nullptr) error.release(&error);

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader next;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &next.stream.value, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(next.GetSchema());
  ASSERT_NO_FATAL_FAILURE(next.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(next.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, EarlyCloseCommitsEarlierStatement) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  for (const char* query : {"DROP TABLE IF EXISTS test_stream_early_close",
                            "CREATE TABLE test_stream_early_close (ints INT)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement,
                  motherduck ? "INSERT INTO test_stream_early_close VALUES (1); "
                               "SELECT i FROM range(3000) t(i)"
                             : "INSERT INTO test_stream_early_close VALUES (1); "
                               "SELECT g FROM generate_series(1, 3000) t(g)",
                  &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader unread;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &unread.stream.value, nullptr, &error),
        IsOkStatus(&error));
  }

  adbc_validation::Handle<struct AdbcConnection> other;
  adbc_validation::Handle<struct AdbcStatement> other_statement;
  ASSERT_THAT(AdbcConnectionNew(&other.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&other.value, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&other.value, &other_statement.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&other_statement.value,
                               "SELECT COUNT(*) FROM test_stream_early_close", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader count;
  ASSERT_THAT(AdbcStatementExecuteQuery(&other_statement.value, &count.stream.value,
                                        nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(count.GetSchema());
  ASSERT_NO_FATAL_FAILURE(count.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(count.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, EarlyCloseLeavesCallerTransactionOpen) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  const bool motherduck = uri != nullptr && std::strstr(uri, "motherduck.com") != nullptr;
  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, motherduck ? "SELECT sleep_ms(2000)" : "SELECT pg_sleep(2)",
                  &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader unread;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &unread.stream.value, nullptr, &error),
        IsOkStatus(&error));
  }

  ASSERT_EQ("intrans", adbc_validation::ConnectionGetOption(
                           &connection, "adbc.postgresql.transaction_status", &error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, ScriptCopyFailsFast) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "COPY (SELECT 1) TO STDOUT; SELECT 2", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader failed;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &failed.stream.value, nullptr, &error),
      ::testing::Not(IsOkStatus(&error)));
  if (error.release != nullptr) error.release(&error);

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, BufferedResultSurvivesNextCommand) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  adbc_validation::Handle<struct AdbcStatement> other;
  ASSERT_THAT(AdbcStatementNew(&connection, &other.value, &error), IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));

  ASSERT_THAT(AdbcStatementSetSqlQuery(&other.value, "SELECT 8", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader next;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&other.value, &next.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(next.GetSchema());
  ASSERT_NO_FATAL_FAILURE(next.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(next.array_view->children[0], 0), 8);

  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, StatementNewLeavesBufferedStream) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 4", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));

  adbc_validation::Handle<struct AdbcStatement> other;
  ASSERT_THAT(AdbcStatementNew(&connection, &other.value, &error), IsOkStatus(&error));

  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 4);
}

TEST_F(PostgresStatementTest, BufferedResultSurvivesConnectionRelease) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 5", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));

  ASSERT_THAT(AdbcConnectionRelease(&connection, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 5);
}

TEST_F(PostgresStatementTest, NextCommandDoesNotJoinUnfinishedScript) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  for (const char* query : {"DROP TABLE IF EXISTS test_stream_held_txn",
                            "CREATE TABLE test_stream_held_txn (ints INT)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "INSERT INTO test_stream_held_txn VALUES (1); "
                                       "SELECT 1",
                                       &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader unread;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &unread.stream.value, nullptr, &error),
      IsOkStatus(&error));

  adbc_validation::Handle<struct AdbcStatement> other;
  ASSERT_THAT(AdbcStatementNew(&connection, &other.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&other.value,
                                       "SELECT * FROM test_stream_held_missing", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader failed;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&other.value, &failed.stream.value, nullptr, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release != nullptr) error.release(&error);

  ASSERT_NO_FATAL_FAILURE(unread.GetSchema());
  ASSERT_NO_FATAL_FAILURE(unread.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(unread.array_view->children[0], 0), 1);

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &other.value, "SELECT COUNT(*) FROM test_stream_held_txn", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader count;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&other.value, &count.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(count.GetSchema());
  ASSERT_NO_FATAL_FAILURE(count.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(count.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, MultipleStatementsDmlVisibleBeforeRead) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "DROP TABLE IF EXISTS test_stream_visible; "
                                       "CREATE TABLE test_stream_visible (ints INT); "
                                       "INSERT INTO test_stream_visible VALUES (1)",
                                       &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader unread;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(&statement, &unread.stream.value, nullptr, &error),
        IsOkStatus(&error));
  }

  adbc_validation::Handle<struct AdbcConnection> other;
  adbc_validation::Handle<struct AdbcStatement> other_statement;
  ASSERT_THAT(AdbcConnectionNew(&other.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&other.value, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&other.value, &other_statement.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&other_statement.value,
                               "SELECT COUNT(*) FROM test_stream_visible", &error),
      IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&other_statement.value, &reader.stream.value,
                                        nullptr, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 1);
}

TEST_F(PostgresStatementTest, NullStreamDoesNotHoldConnection) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 2", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 2);
}

TEST_F(PostgresStatementTest, MultipleStatementsStayAtomic) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  for (const char* query : {"DROP TABLE IF EXISTS test_stream_atomic",
                            "CREATE TABLE test_stream_atomic (ints INT)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "INSERT INTO test_stream_atomic VALUES (1); "
                                       "SELECT * FROM test_stream_atomic_missing",
                                       &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader failed;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &failed.stream.value, nullptr, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_EQ("42P01", std::string_view(error.sqlstate, 5));
  error.release(&error);

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT COUNT(*) FROM test_stream_atomic", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(
      AdbcStatementExecuteQuery(&statement, &reader.stream.value, nullptr, &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0), 0);
}

TEST_F(PostgresStatementTest, MultipleStatementsSingleQueryError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // The failing statement reports its own error and stops the ones after it.
  const char* query = R"(DROP TABLE IF EXISTS test_query_statements_error;
            SELECT * FROM test_query_statements_missing;
            CREATE TABLE test_query_statements_error (ints INT);)";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  ASSERT_EQ("42P01", std::string_view(error.sqlstate, 5));
  error.release(&error);

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT * FROM test_query_statements_error", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  error.release(&error);
}

TEST_F(PostgresStatementTest, MultipleStatementsSingleQueryQuoting) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // Semicolons inside literals don't split statements, and the batch reports
  // the last statement's row count.
  const char* query = R"(DROP TABLE IF EXISTS test_query_statements_quoting;
            CREATE TABLE test_query_statements_quoting (strs TEXT);
            INSERT INTO test_query_statements_quoting VALUES ('a;b'), ('it''s; here');)";
  int64_t rows_affected = 0;
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, &rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_EQ(2, rows_affected);

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT * FROM test_query_statements_quoting "
                                       "WHERE strs IN ('a;b', 'it''s; here')",
                                       &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  reader.GetSchema();
  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->length, 2);
}

TEST_F(PostgresStatementTest, MultipleStatementsSingleQueryCopy) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // A COPY in a batch fails cleanly and leaves the connection usable.
  for (const char* query : {"SELECT 1; COPY (SELECT 1) TO STDOUT",
                            "DROP TABLE IF EXISTS test_query_statements_copy; "
                            "CREATE TABLE test_query_statements_copy (ints INT); "
                            "COPY test_query_statements_copy FROM STDIN"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                ::testing::Not(IsOkStatus(&error)));
    error.release(&error);
  }

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT 1; SELECT 2", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));
}

// Bind values as the single BIGINT parameter $1, one row per value.
void BindInt64Params(struct AdbcStatement* statement,
                     const std::vector<std::optional<int64_t>>& values,
                     struct AdbcError* error) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ASSERT_THAT(
      adbc_validation::MakeSchema(&schema.value, {{"ints", NANOARROW_TYPE_INT64}}),
      adbc_validation::IsOkErrno());
  ASSERT_THAT(
      adbc_validation::MakeBatch<int64_t>(
          &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr), values),
      adbc_validation::IsOkErrno());
  ASSERT_THAT(AdbcStatementBind(statement, &batch.value, &schema.value, error),
              IsOkStatus(error));
}

void ExecuteEach(struct AdbcStatement* statement, std::vector<const char*> queries,
                 struct AdbcError* error) {
  for (const char* query : queries) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(statement, query, error), IsOkStatus(error));
    ASSERT_THAT(AdbcStatementExecuteQuery(statement, nullptr, nullptr, error),
                IsOkStatus(error));
  }
}

// Read the first column of a single-row query's result as text.
void QueryScalar(struct AdbcStatement* statement, const char* query, std::string* out,
                 struct AdbcError* error) {
  ASSERT_THAT(AdbcStatementSetSqlQuery(statement, query, error), IsOkStatus(error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(statement, &reader.stream.value,
                                        &reader.rows_affected, error),
              IsOkStatus(error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_EQ(1, reader.array->length);
  struct ArrowStringView value =
      ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
  out->assign(value.data, static_cast<size_t>(value.size_bytes));
}

// motherduck.db_schema starts every connection in that schema of the
// connection's database, and connecting fails when the schema doesn't exist,
// even when an attached database has its name (a bare search_path entry would
// switch to that database instead).
TEST_F(PostgresDatabaseTest, DbSchemaOption) {
  if (RunSchema().empty()) {
    GTEST_SKIP() << "Needs MOTHERDUCK_TEST_RUN (see ci/scripts/pre-test.sh)";
  }
  const std::string catalog = quirks_.catalog();
  // A schema of the test database.
  const std::string name = TestSchema("db_schema_option");
  // A database, with no schema of that name in the test database.
  const std::string database_only = TestSchema("db_schema_option_db");
  const std::string quoted_database_only = quirks_.QuoteIdentifier(database_only);

  // quirks_ sets the option to the run's schema.
  Handle<struct AdbcDatabase> workspace;
  ASSERT_THAT(AdbcDatabaseNew(&workspace.value, &error), IsOkStatus(&error));
  ASSERT_THAT(quirks_.SetupDatabase(&workspace.value, &error), IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &workspace.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA, &error),
              ::testing::Optional(TestSchema()));
  ASSERT_THAT(AdbcDatabaseSetOption(&workspace.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                    "workspace", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseInit(&workspace.value, &error), IsOkStatus(&error));
  ASSERT_THAT(ExecuteSql(&workspace.value,
                         "CREATE SCHEMA IF NOT EXISTS " + quirks_.QuoteIdentifier(name) +
                             "; CREATE DATABASE IF NOT EXISTS " + quoted_database_only,
                         &error),
              IsOkStatus(&error));

  {
    Handle<struct AdbcDatabase> database;
    Handle<struct AdbcConnection> connection;
    Handle<struct AdbcStatement> statement;
    ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupDatabase(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      "workspace", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA,
                                      name.c_str(), &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseInit(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionInit(&connection.value, &database.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
                IsOkStatus(&error));
    std::string current;
    ASSERT_NO_FATAL_FAILURE(QueryScalar(
        &statement.value, "SELECT current_database() || '.' || current_schema()",
        &current, &error));
    EXPECT_EQ(current, catalog + "." + name);

    // The connection option, set before Init (as Python does), replaces it.
    {
      Handle<struct AdbcConnection> other;
      Handle<struct AdbcStatement> other_statement;
      ASSERT_THAT(AdbcConnectionNew(&other.value, &error), IsOkStatus(&error));
      ASSERT_THAT(
          AdbcConnectionSetOption(&other.value, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                                  TestSchema().c_str(), &error),
          IsOkStatus(&error));
      ASSERT_THAT(AdbcConnectionInit(&other.value, &database.value, &error),
                  IsOkStatus(&error));
      ASSERT_THAT(AdbcStatementNew(&other.value, &other_statement.value, &error),
                  IsOkStatus(&error));
      ASSERT_NO_FATAL_FAILURE(QueryScalar(
          &other_statement.value, "SELECT current_database() || '.' || current_schema()",
          &current, &error));
      EXPECT_EQ(current, catalog + "." + TestSchema());
    }

    // The connection option resolves the same way: a name only an attached
    // database has fails, rather than switching to that database, with the
    // server's SQLSTATE.
    EXPECT_NE(AdbcConnectionSetOption(&connection.value,
                                      ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
                                      database_only.c_str(), &error),
              ADBC_STATUS_OK);
    EXPECT_NE(error.sqlstate[0], '\0');
    ASSERT_NE(nullptr, error.message);
    EXPECT_THAT(error.message,
                ::testing::HasSubstr(ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA));
    if (error.release) error.release(&error);
    ASSERT_NO_FATAL_FAILURE(QueryScalar(
        &statement.value, "SELECT current_database() || '.' || current_schema()",
        &current, &error));
    EXPECT_EQ(current, catalog + "." + name);

    // An empty value unsets it.
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA,
                                      "", &error),
                IsOkStatus(&error));
    char value[128];
    size_t length = sizeof(value);
    EXPECT_THAT(AdbcDatabaseGetOption(&database.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA,
                                      value, &length, &error),
                IsStatus(ADBC_STATUS_NOT_FOUND, &error));
    if (error.release) error.release(&error);
  }

  {
    // A URI that names no database (md:) leaves the current database to the
    // server, which the driver looks up to qualify the schema.
    const char* setup_uri = std::getenv("MOTHERDUCK_TEST_SETUP_URI");
    ASSERT_NE(setup_uri, nullptr) << "Must provide env var MOTHERDUCK_TEST_SETUP_URI";
    Handle<struct AdbcDatabase> md;
    Handle<struct AdbcConnection> md_connection;
    Handle<struct AdbcStatement> md_statement;
    ASSERT_THAT(AdbcDatabaseNew(&md.value, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupDatabase(&md.value, setup_uri, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&md.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      "workspace", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&md.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA, "main",
                                      &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseInit(&md.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionNew(&md_connection.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcConnectionInit(&md_connection.value, &md.value, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementNew(&md_connection.value, &md_statement.value, &error),
                IsOkStatus(&error));
    std::string md_current;
    ASSERT_NO_FATAL_FAILURE(
        QueryScalar(&md_statement.value, "SELECT current_schema()", &md_current, &error));
    EXPECT_EQ(md_current, "main");
  }

  {
    Handle<struct AdbcDatabase> missing;
    ASSERT_THAT(AdbcDatabaseNew(&missing.value, &error), IsOkStatus(&error));
    ASSERT_THAT(quirks_.SetupDatabase(&missing.value, &error), IsOkStatus(&error));
    // Workspace mode attaches the database with the name.
    ASSERT_THAT(AdbcDatabaseSetOption(&missing.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      "workspace", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&missing.value, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA,
                                      database_only.c_str(), &error),
                IsOkStatus(&error));
    // Fails with the server's SQLSTATE.
    ASSERT_NE(AdbcDatabaseInit(&missing.value, &error), ADBC_STATUS_OK);
    EXPECT_NE(error.sqlstate[0], '\0');
    ASSERT_NE(nullptr, error.message);
    EXPECT_THAT(error.message, ::testing::HasSubstr(ADBC_MOTHERDUCK_OPTION_DB_SCHEMA));
    if (error.release) error.release(&error);
  }

  EXPECT_THAT(ExecuteSql(&workspace.value,
                         "DROP DATABASE IF EXISTS " + quoted_database_only, &error),
              IsOkStatus(&error));
}

// PostgreSQL rolls back a pipeline's implicit transaction when a command in it
// fails; MotherDuck keeps what ran before the failure. MotherDuck's version()
// also starts with "PostgreSQL", so go by the server the tests connect to.
bool RollsBackFailedPipelines() {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  return uri == nullptr || std::strstr(uri, "motherduck.com") == nullptr;
}

void CountRows(struct AdbcStatement* statement, const std::string& table,
               std::string* out, struct AdbcError* error) {
  const std::string query = "SELECT CAST(count(*) AS TEXT) FROM " + table;
  ASSERT_NO_FATAL_FAILURE(QueryScalar(statement, query.c_str(), out, error));
}

// A batch with one UTC TIMESTAMP column, ts, holding the given microseconds.
void MakeTimestampTzBatch(struct ArrowSchema* schema, struct ArrowArray* batch,
                          const std::vector<std::optional<int64_t>>& values) {
  ArrowSchemaInit(schema);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(schema, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP,
                                         NANOARROW_TIME_UNIT_MICRO, "UTC"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "ts"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT((adbc_validation::MakeBatch<int64_t>(
                  schema, batch, static_cast<struct ArrowError*>(nullptr), values)),
              adbc_validation::IsOkErrno());
}

// A bind stream that yields one batch of $1 values and then fails, as a
// broken upstream reader would. The values are BIGINTs, or with timestamptz,
// UTC timestamps in microseconds.
struct FailingBindStream {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  bool sent_batch = false;

  static void Make(const std::vector<std::optional<int64_t>>& values,
                   struct ArrowArrayStream* out, bool timestamptz = false) {
    auto* data = new FailingBindStream();
    if (timestamptz) {
      ASSERT_NO_FATAL_FAILURE(
          MakeTimestampTzBatch(&data->schema.value, &data->batch.value, values));
    } else {
      ASSERT_THAT(adbc_validation::MakeSchema(&data->schema.value,
                                              {{"ints", NANOARROW_TYPE_INT64}}),
                  adbc_validation::IsOkErrno());
      ASSERT_THAT(adbc_validation::MakeBatch<int64_t>(
                      &data->schema.value, &data->batch.value,
                      static_cast<struct ArrowError*>(nullptr), values),
                  adbc_validation::IsOkErrno());
    }
    out->private_data = data;
    out->get_schema = [](struct ArrowArrayStream* stream, struct ArrowSchema* schema) {
      auto* data = static_cast<FailingBindStream*>(stream->private_data);
      return ArrowSchemaDeepCopy(&data->schema.value, schema);
    };
    out->get_next = [](struct ArrowArrayStream* stream, struct ArrowArray* array) {
      auto* data = static_cast<FailingBindStream*>(stream->private_data);
      if (data->sent_batch) return EIO;
      data->sent_batch = true;
      ArrowArrayMove(&data->batch.value, array);
      return 0;
    };
    out->get_last_error = [](struct ArrowArrayStream*) -> const char* {
      return "the bind stream failed";
    };
    out->release = [](struct ArrowArrayStream* stream) {
      delete static_cast<FailingBindStream*>(stream->private_data);
      stream->release = nullptr;
    };
  }
};

TEST_F(PostgresStatementTest, ExecuteManyRows) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(&statement,
                                      {"DROP TABLE IF EXISTS test_execute_many",
                                       "CREATE TABLE test_execute_many (ints BIGINT)"},
                                      &error));

  // MotherDuck spends ~20 ms on each INSERT, so keep this small.
  constexpr int64_t kRows = 500;
  std::vector<std::optional<int64_t>> values;
  for (int64_t i = 0; i < kRows; i++) values.push_back(i);
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "INSERT INTO test_execute_many VALUES ($1)", &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(BindInt64Params(&statement, values, &error));
  int64_t rows_affected = 0;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, &rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_EQ(kRows, rows_affected);

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT ints FROM test_execute_many ORDER BY ints", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  int64_t seen = 0;
  while (true) {
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    if (!reader.array->release) break;
    for (int64_t i = 0; i < reader.array->length; i++) {
      ASSERT_EQ(seen, ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], i));
      seen++;
    }
  }
  ASSERT_EQ(kRows, seen);
}

TEST_F(PostgresStatementTest, ExecuteManyLargeResults) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // About 32 MB of results that nothing reads: enough to fill both ends'
  // socket buffers, so the driver must read while it's still sending rows.
  std::vector<std::optional<int64_t>> values;
  for (int64_t i = 0; i < 500; i++) values.push_back(i);
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "SELECT CAST($1 AS BIGINT) + 1, repeat('x', 65536)", &error),
      IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(BindInt64Params(&statement, values, &error));

  // A deadlock would hang here, so fail fast rather than at ctest's timeout.
  // The stuck worker can't be joined or safely detached, so exit outright;
  // gtest prints the failure as it's recorded.
  std::promise<AdbcStatusCode> executed;
  std::future<AdbcStatusCode> code = executed.get_future();
  std::thread worker([&] {
    executed.set_value(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error));
  });
  if (code.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
    ADD_FAILURE() << "No result after 120 s; the pipeline deadlocked?";
    std::_Exit(1);
  }
  worker.join();
  ASSERT_THAT(code.get(), IsOkStatus(&error));
}

TEST_F(PostgresStatementTest, ExecuteManyError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(
      ExecuteEach(&statement,
                  {"DROP TABLE IF EXISTS test_execute_many_error",
                   "CREATE TABLE test_execute_many_error (ints BIGINT PRIMARY KEY)"},
                  &error));

  // The second row breaks the primary key; the error comes back and the
  // connection stays usable.
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "INSERT INTO test_execute_many_error VALUES ($1)", &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(BindInt64Params(&statement, {1, 1, 2}, &error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ::testing::Not(IsOkStatus(&error)));
  error.release(&error);

  // The rows share one implicit transaction, so the first row goes too.
  std::string rows;
  ASSERT_NO_FATAL_FAILURE(
      CountRows(&statement, "test_execute_many_error", &rows, &error));
  if (RollsBackFailedPipelines()) ASSERT_EQ("0", rows);
}

TEST_F(PostgresStatementTest, ExecuteManyStreamError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(
      ExecuteEach(&statement,
                  {"DROP TABLE IF EXISTS test_execute_many_stream_error",
                   "CREATE TABLE test_execute_many_stream_error (ints BIGINT)"},
                  &error));

  // The first batch goes out before the stream fails; like a failed row, the
  // failure keeps those rows from being committed.
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "INSERT INTO test_execute_many_stream_error VALUES ($1)", &error),
      IsOkStatus(&error));
  struct ArrowArrayStream stream = {};
  ASSERT_NO_FATAL_FAILURE(FailingBindStream::Make({1, 2, 3}, &stream));
  ASSERT_THAT(AdbcStatementBindStream(&statement, &stream, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ::testing::Not(IsOkStatus(&error)));
  ASSERT_THAT(error.message, ::testing::HasSubstr("the bind stream failed"));
  error.release(&error);

  std::string rows;
  ASSERT_NO_FATAL_FAILURE(
      CountRows(&statement, "test_execute_many_stream_error", &rows, &error));
  if (RollsBackFailedPipelines()) ASSERT_EQ("0", rows);
}

TEST_F(PostgresStatementTest, ExecuteManyTimestampTzError) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(
      &statement,
      {"DROP TABLE IF EXISTS test_execute_many_tz_error",
       "CREATE TABLE test_execute_many_tz_error (ts TIMESTAMPTZ PRIMARY KEY)"},
      &error));
  std::string time_zone;
  ASSERT_NO_FATAL_FAILURE(QueryScalar(&statement,
                                      "SELECT CAST(current_setting('TimeZone') AS TEXT)",
                                      &time_zone, &error));

  // Binding a TIMESTAMPTZ under autocommit runs inside a transaction that sets
  // the time zone to UTC. When a row fails, both are undone and the
  // connection is left usable.
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ASSERT_NO_FATAL_FAILURE(MakeTimestampTzBatch(&schema.value, &batch.value, {0, 0}));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(
          &statement, "INSERT INTO test_execute_many_tz_error VALUES ($1)", &error),
      IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ::testing::Not(IsOkStatus(&error)));
  error.release(&error);

  std::string time_zone_after;
  ASSERT_NO_FATAL_FAILURE(QueryScalar(&statement,
                                      "SELECT CAST(current_setting('TimeZone') AS TEXT)",
                                      &time_zone_after, &error));
  ASSERT_EQ(time_zone, time_zone_after);
  std::string rows;
  ASSERT_NO_FATAL_FAILURE(
      CountRows(&statement, "test_execute_many_tz_error", &rows, &error));
  ASSERT_EQ("0", rows);
}

// After a failed bind that set the time zone to UTC, check that the session's
// America/New_York is back and that the connection commits again. A
// transaction left open would keep an insert from committing, which only
// another connection can see.
void ExpectBindUndone(struct AdbcDatabase* database, struct AdbcStatement* statement,
                      const std::string& table, struct AdbcError* error) {
  std::string time_zone;
  ASSERT_NO_FATAL_FAILURE(QueryScalar(
      statement, "SELECT CAST(current_setting('TimeZone') AS TEXT)", &time_zone, error));
  ASSERT_EQ("America/New_York", time_zone);
  const std::string insert = "INSERT INTO " + table + " VALUES (1)";
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(statement, {insert.c_str()}, error));
  adbc_validation::Handle<struct AdbcConnection> other;
  adbc_validation::Handle<struct AdbcStatement> other_statement;
  ASSERT_THAT(AdbcConnectionNew(&other.value, error), IsOkStatus(error));
  ASSERT_THAT(AdbcConnectionInit(&other.value, database, error), IsOkStatus(error));
  ASSERT_THAT(AdbcStatementNew(&other.value, &other_statement.value, error),
              IsOkStatus(error));
  std::string rows;
  ASSERT_NO_FATAL_FAILURE(CountRows(&other_statement.value, table, &rows, error));
  ASSERT_EQ("1", rows);
}

TEST_F(PostgresStatementTest, PrepareErrorRestoresTimeZone) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // Binding a TIMESTAMPTZ under autocommit begins a transaction and sets the
  // time zone to UTC before the query is prepared. When the prepare fails, both
  // must be undone. MotherDuck's ROLLBACK leaves the time zone alone, so this
  // also checks that the driver sets it back itself. Without a result stream
  // the driver runs every row up front (ExecuteAll); with one it prepares in
  // Initialize().
  for (bool with_stream : {false, true}) {
    SCOPED_TRACE(with_stream ? "with a result stream" : "without a result stream");
    ASSERT_NO_FATAL_FAILURE(ExecuteEach(&statement,
                                        {"DROP TABLE IF EXISTS test_prepare_error_tz",
                                         "CREATE TABLE test_prepare_error_tz (x INT)",
                                         "SET TimeZone = 'America/New_York'"},
                                        &error));
    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> batch;
    ASSERT_NO_FATAL_FAILURE(MakeTimestampTzBatch(&schema.value, &batch.value, {0}));
    ASSERT_THAT(AdbcStatementSetSqlQuery(
                    &statement,
                    "SELECT * FROM test_prepare_error_tz_missing WHERE ts = $1", &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
                IsOkStatus(&error));
    adbc_validation::StreamReader reader;
    ASSERT_THAT(
        AdbcStatementExecuteQuery(
            &statement, with_stream ? &reader.stream.value : nullptr, nullptr, &error),
        ::testing::Not(IsOkStatus(&error)));
    error.release(&error);

    ASSERT_NO_FATAL_FAILURE(
        ExpectBindUndone(&database, &statement, "test_prepare_error_tz", &error));
  }
}

TEST_F(PostgresStatementTest, DecodeErrorRestoresTimeZone) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(
      &statement,
      {"DROP TABLE IF EXISTS test_decode_error_tz",
       "CREATE TABLE test_decode_error_tz (x INT)", "SET TimeZone = 'America/New_York'"},
      &error));

  // The query runs, but the interval overflows nanoseconds, so reading the
  // result fails. That must undo the bind like an error from the server.
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ASSERT_NO_FATAL_FAILURE(MakeTimestampTzBatch(&schema.value, &batch.value, {0}));
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT INTERVAL '3000000 hours' AS v "
                                       "WHERE CAST($1 AS TIMESTAMPTZ) IS NOT NULL",
                                       &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NE(0, reader.MaybeNext());
    // The failure reaches the error detail, and a retry fails again instead of
    // ending the stream, though the bind was rolled back.
    AdbcStatusCode code = ADBC_STATUS_OK;
    const struct AdbcError* detail =
        AdbcErrorFromArrayStream(&reader.stream.value, &code);
    ASSERT_NE(nullptr, detail);
    ASSERT_NE(ADBC_STATUS_OK, code);
    ASSERT_NE(nullptr, detail->message);
    ASSERT_NE(0, reader.MaybeNext());
  }

  ASSERT_NO_FATAL_FAILURE(
      ExpectBindUndone(&database, &statement, "test_decode_error_tz", &error));
}

// Run a two-row TIMESTAMPTZ bind with a result stream under autocommit and
// read the first batch, leaving the bind stream running. The bind has begun a
// transaction and set the time zone to UTC.
void StartTimestampTzStream(struct AdbcStatement* statement, const char* table,
                            adbc_validation::StreamReader* reader,
                            struct AdbcError* error) {
  const std::string drop = std::string("DROP TABLE IF EXISTS ") + table;
  const std::string create = std::string("CREATE TABLE ") + table + " (x INT)";
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(
      statement, {drop.c_str(), create.c_str(), "SET TimeZone = 'America/New_York'"},
      error));
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ASSERT_NO_FATAL_FAILURE(MakeTimestampTzBatch(&schema.value, &batch.value, {0, 1}));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(statement, "SELECT CAST($1 AS TIMESTAMPTZ) AS v", error),
      IsOkStatus(error));
  ASSERT_THAT(AdbcStatementBind(statement, &batch.value, &schema.value, error),
              IsOkStatus(error));
  ASSERT_THAT(AdbcStatementExecuteQuery(statement, &reader->stream.value,
                                        &reader->rows_affected, error),
              IsOkStatus(error));
  ASSERT_NO_FATAL_FAILURE(reader->GetSchema());
  ASSERT_NO_FATAL_FAILURE(reader->Next());
  ASSERT_EQ(reader->array->length, 1);
}

TEST_F(PostgresStatementTest, ReleaseEarlyRestoresTimeZone) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_NO_FATAL_FAILURE(
        StartTimestampTzStream(&statement, "test_release_early_tz", &reader, &error));
    // The reader is released with the second bind row still to run.
  }
  ASSERT_NO_FATAL_FAILURE(
      ExpectBindUndone(&database, &statement, "test_release_early_tz", &error));
}

TEST_F(PostgresStatementTest, CancelMidStreamRestoresTimeZone) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_NO_FATAL_FAILURE(
        StartTimestampTzStream(&statement, "test_cancel_mid_tz", &reader, &error));
    ASSERT_THAT(AdbcStatementCancel(&statement, &error), IsOkStatus(&error));
    ASSERT_EQ(ECANCELED, reader.MaybeNext());
  }
  ASSERT_NO_FATAL_FAILURE(
      ExpectBindUndone(&database, &statement, "test_cancel_mid_tz", &error));
}

// When the bind stream fails after the first batch was returned, the bind is
// rolled back. The failure has to reach the error detail and survive a retry,
// not turn into a clean end of stream.
TEST_F(PostgresStatementTest, BindErrorMidStreamFailsStickily) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(
      &statement,
      {"DROP TABLE IF EXISTS test_bind_error_mid",
       "CREATE TABLE test_bind_error_mid (x INT)", "SET TimeZone = 'America/New_York'"},
      &error));
  ASSERT_THAT(
      AdbcStatementSetSqlQuery(&statement, "SELECT CAST($1 AS TIMESTAMPTZ) AS v", &error),
      IsOkStatus(&error));
  struct ArrowArrayStream stream = {};
  ASSERT_NO_FATAL_FAILURE(FailingBindStream::Make({0}, &stream, /*timestamptz=*/true));
  ASSERT_THAT(AdbcStatementBindStream(&statement, &stream, &error), IsOkStatus(&error));
  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                          &reader.rows_affected, &error),
                IsOkStatus(&error));
    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(reader.array->length, 1);

    const int code = reader.MaybeNext();
    ASSERT_NE(0, code);
    AdbcStatusCode status = ADBC_STATUS_OK;
    const struct AdbcError* detail =
        AdbcErrorFromArrayStream(&reader.stream.value, &status);
    ASSERT_NE(nullptr, detail);
    ASSERT_NE(ADBC_STATUS_OK, status);
    ASSERT_THAT(detail->message, ::testing::HasSubstr("the bind stream failed"));
    ASSERT_EQ(code, reader.MaybeNext());
    ASSERT_NE(ADBC_STATUS_OK, status);
  }
  ASSERT_NO_FATAL_FAILURE(
      ExpectBindUndone(&database, &statement, "test_bind_error_mid", &error));
}

TEST_F(PostgresStatementTest, BindStreamErrorRestoresTimeZoneWithoutAutocommit) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(
      ExecuteEach(&statement,
                  {"DROP TABLE IF EXISTS test_bind_error_tz_txn",
                   "CREATE TABLE test_bind_error_tz_txn (ts TIMESTAMPTZ)"},
                  &error));
  ASSERT_THAT(AdbcConnectionSetOption(&connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                      ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(
      ExecuteEach(&statement, {"SET TimeZone = 'America/New_York'"}, &error));

  // With autocommit off the transaction is the user's. When binding fails in
  // the driver (here the bind stream), the time zone must be restored and the
  // transaction left open and usable.
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "INSERT INTO test_bind_error_tz_txn VALUES ($1)", &error),
              IsOkStatus(&error));
  struct ArrowArrayStream stream = {};
  ASSERT_NO_FATAL_FAILURE(FailingBindStream::Make({0}, &stream, /*timestamptz=*/true));
  ASSERT_THAT(AdbcStatementBindStream(&statement, &stream, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              ::testing::Not(IsOkStatus(&error)));
  ASSERT_THAT(error.message, ::testing::HasSubstr("the bind stream failed"));
  error.release(&error);

  std::string time_zone;
  ASSERT_NO_FATAL_FAILURE(QueryScalar(&statement,
                                      "SELECT CAST(current_setting('TimeZone') AS TEXT)",
                                      &time_zone, &error));
  ASSERT_EQ("America/New_York", time_zone);
  ASSERT_THAT(AdbcConnectionCommit(&connection, &error), IsOkStatus(&error));

  // The stream sent its row before it failed. Rolling the user's transaction
  // back would drop it, and Commit succeeds with nothing open, so count the
  // row from another connection.
  adbc_validation::Handle<struct AdbcConnection> other;
  adbc_validation::Handle<struct AdbcStatement> other_statement;
  ASSERT_THAT(AdbcConnectionNew(&other.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcConnectionInit(&other.value, &database, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&other.value, &other_statement.value, &error),
              IsOkStatus(&error));
  std::string rows;
  ASSERT_NO_FATAL_FAILURE(
      CountRows(&other_statement.value, "test_bind_error_tz_txn", &rows, &error));
  ASSERT_EQ("1", rows);
}

TEST_F(PostgresStatementTest, IngestReplaceKeepsTableOnBadColumnName) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(ExecuteEach(&statement,
                                      {"DROP TABLE IF EXISTS test_ingest_bad_name",
                                       "CREATE TABLE test_ingest_bad_name (ints BIGINT)",
                                       "INSERT INTO test_ingest_bad_name VALUES (1)"},
                                      &error));

  // A column name that isn't valid UTF-8 can't be escaped. Replace mode must
  // fail on it before dropping the existing table.
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "\xff"),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(
      (adbc_validation::MakeBatch<int64_t>(
          &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr), {2})),
      adbc_validation::IsOkErrno());
  adbc_validation::Handle<struct AdbcStatement> ingest;
  ASSERT_THAT(AdbcStatementNew(&connection, &ingest.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&ingest.value, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "test_ingest_bad_name", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetOption(&ingest.value, ADBC_INGEST_OPTION_MODE,
                                     ADBC_INGEST_OPTION_MODE_REPLACE, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&ingest.value, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_EQ(ADBC_STATUS_INVALID_ARGUMENT,
            AdbcStatementExecuteQuery(&ingest.value, nullptr, nullptr, &error));
  error.release(&error);

  std::string rows;
  ASSERT_NO_FATAL_FAILURE(CountRows(&statement, "test_ingest_bad_name", &rows, &error));
  ASSERT_EQ("1", rows);
}

TEST_F(PostgresStatementTest, SetUseCopyFalse) {
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // One statement per query; MultipleStatementsSingleQuery covers batches.
  for (const char* query :
       {"DROP TABLE IF EXISTS test_query_set_copy_false",
        "CREATE TABLE test_query_set_copy_false (ints INT)",
        "INSERT INTO test_query_set_copy_false VALUES (1), (NULL), (3)"}) {
    ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
                IsOkStatus(&error));
  }

  // Check option setting/getting; the test setup turns COPY off.
  ASSERT_EQ(
      adbc_validation::StatementGetOption(&statement, "adbc.postgresql.use_copy", &error),
      "false");

  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.use_copy",
                                     "not true or false", &error),
              IsStatus(ADBC_STATUS_INVALID_ARGUMENT));

  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.use_copy",
                                     ADBC_OPTION_VALUE_ENABLED, &error),
              IsOkStatus(&error));
  ASSERT_EQ(
      adbc_validation::StatementGetOption(&statement, "adbc.postgresql.use_copy", &error),
      "true");

  ASSERT_THAT(AdbcStatementSetOption(&statement, "adbc.postgresql.use_copy",
                                     ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
  ASSERT_EQ(
      adbc_validation::StatementGetOption(&statement, "adbc.postgresql.use_copy", &error),
      "false");

  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement,
                                       "SELECT * FROM test_query_set_copy_false", &error),
              IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));

  // The result is streamed, so the count is not known when ExecuteQuery returns.
  ASSERT_EQ(reader.rows_affected, -1);

  reader.GetSchema();
  ASSERT_EQ(reader.schema->n_children, 1);
  ASSERT_STREQ(reader.schema->children[0]->format, "i");
  ASSERT_STREQ(reader.schema->children[0]->name, "ints");

  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->length, 3);
  ASSERT_EQ(reader.array->n_children, 1);
  ASSERT_EQ(reader.array->children[0]->null_count, 1);

  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->release, nullptr);
}

TEST_F(PostgresStatementTest, SqlQueryInt2vector) {
  GTEST_SKIP() << "int2vector is PostgreSQL-only";
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  const char* query = R"(SELECT CAST('-1 42 0' AS int2vector) AS thevector;)";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, query, &error), IsOkStatus(&error));

  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));

  reader.GetSchema();
  ASSERT_EQ(reader.schema->n_children, 1);
  ASSERT_STREQ(reader.schema->children[0]->format, "+l");
  ASSERT_STREQ(reader.schema->children[0]->name, "thevector");
  ASSERT_EQ(reader.schema->children[0]->n_children, 1);
  ASSERT_STREQ(reader.schema->children[0]->children[0]->format, "s");

  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->length, 1);
  ASSERT_EQ(reader.array->n_children, 1);
  ASSERT_EQ(reader.array->children[0]->null_count, 0);
  const auto* offsets =
      reinterpret_cast<const int32_t*>(reader.array->children[0]->buffers[1]);
  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 3);

  ASSERT_EQ(reader.array->children[0]->children[0]->null_count, 0);
  ASSERT_EQ(reader.array->children[0]->children[0]->length, 3);
  const auto* data = reinterpret_cast<const int16_t*>(
      reader.array->children[0]->children[0]->buffers[1]);
  ASSERT_EQ(data[0], -1);
  ASSERT_EQ(data[1], 42);
  ASSERT_EQ(data[2], 0);

  ASSERT_THAT(reader.MaybeNext(), adbc_validation::IsOkErrno());
  ASSERT_EQ(reader.array->release, nullptr);
}

TEST_F(PostgresStatementTest, UnknownOid) {
  GTEST_SKIP() << "Reads PostgreSQL-only pg_type.typacl";
  // Regression test for https://github.com/apache/arrow-adbc/issues/2448
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "SELECT typacl FROM pg_type WHERE oid <= 6157", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(1, reader.fields.size());
  ASSERT_EQ(NANOARROW_TYPE_BINARY, reader.fields[0].type);
  struct ArrowStringView extension_name = reader.fields[0].extension_name;
  ASSERT_EQ("arrow.opaque",
            std::string_view(extension_name.data,
                             static_cast<size_t>(extension_name.size_bytes)));
  struct ArrowStringView extension_metadata = reader.fields[0].extension_metadata;
  ASSERT_EQ(R"({"type_name": "unnamed<oid:1034>", "vendor_name": "PostgreSQL"})",
            std::string_view(extension_metadata.data,
                             static_cast<size_t>(extension_metadata.size_bytes)));
}

TEST_F(PostgresStatementTest, SqlQueryJsonb) {
  GTEST_SKIP() << "JSONB is PostgreSQL-only";
  ASSERT_THAT(AdbcStatementNew(&connection, &statement, &error), IsOkStatus(&error));

  // Setup table
  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement,
                  "DROP TABLE IF EXISTS jsonbtest; CREATE TABLE jsonbtest (value JSONB);",
                  &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  // Insert data
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> batch;
  ArrowSchemaInit(&schema.value);
  ASSERT_THAT(ArrowSchemaSetTypeStruct(&schema.value, 1), adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_STRING),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(ArrowSchemaSetName(schema->children[0], "value"),
              adbc_validation::IsOkErrno());

  // We need the extension type for the driver to bind data properly
  nanoarrow::UniqueBuffer buffer;
  ASSERT_THAT(ArrowMetadataBuilderInit(buffer.get(), nullptr),
              adbc_validation::IsOkErrno());
  ASSERT_THAT(
      ArrowMetadataBuilderAppend(buffer.get(), ArrowCharView("ARROW:extension:name"),
                                 ArrowCharView("arrow.json")),
      adbc_validation::IsOkErrno());
  ASSERT_THAT(
      ArrowSchemaSetMetadata(schema->children[0], reinterpret_cast<char*>(buffer->data)),
      adbc_validation::IsOkErrno());

  ASSERT_THAT((adbc_validation::MakeBatch<std::string>(
                  &schema.value, &batch.value, static_cast<struct ArrowError*>(nullptr),
                  {R"({"a": 1, "b": [1, 2, 3]})", std::nullopt})),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement, "INSERT INTO jsonbtest(value) VALUES ($1)", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementBind(&statement, &batch.value, &schema.value, &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, nullptr, nullptr, &error),
              IsOkStatus(&error));

  // Check round-trip
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement, "SELECT * FROM jsonbtest", &error),
              IsOkStatus(&error));
  adbc_validation::StreamReader reader;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement, &reader.stream.value,
                                        &reader.rows_affected, &error),
              IsOkStatus(&error));

  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_EQ(1, reader.fields.size());
  ASSERT_EQ(NANOARROW_TYPE_STRING, reader.fields[0].type);
  ASSERT_EQ(
      "arrow.json",
      std::string_view(reader.fields[0].extension_name.data,
                       static_cast<size_t>(reader.fields[0].extension_name.size_bytes)));

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ArrowStringView view = ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
  std::string_view v(view.data, static_cast<size_t>(view.size_bytes));
  ASSERT_EQ(R"({"a": 1, "b": [1, 2, 3]})", v);
}

struct TypeTestCase {
  std::string name;
  std::string sql_type;
  std::string sql_literal;
  ArrowType arrow_type;
  std::variant<bool, int64_t, double, std::string, ArrowInterval> scalar;

  static std::string FormatName(const ::testing::TestParamInfo<TypeTestCase>& info) {
    return info.param.name;
  }
};

ArrowInterval MonthDayNano(int32_t months, int32_t days, int64_t nanos) {
  return {NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO, months, days, 0, nanos};
}

void PrintTo(const TypeTestCase& value, std::ostream* os) { (*os) << value.name; }

class PostgresTypeTest : public ::testing::TestWithParam<TypeTestCase> {
 public:
  void SetUp() override {
    ASSERT_THAT(AdbcDatabaseNew(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(quirks_.SetupDatabase(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcDatabaseInit(&database_, &error_), IsOkStatus(&error_));

    ASSERT_THAT(AdbcConnectionNew(&connection_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcConnectionInit(&connection_, &database_, &error_),
                IsOkStatus(&error_));

    ASSERT_THAT(AdbcStatementNew(&connection_, &statement_, &error_),
                IsOkStatus(&error_));

    ASSERT_THAT(quirks_.DropTable(&connection_, "foo", &error_), IsOkStatus(&error_));
  }
  void TearDown() override {
    if (statement_.private_data) {
      ASSERT_THAT(AdbcStatementRelease(&statement_, &error_), IsOkStatus(&error_));
    }
    if (connection_.private_data) {
      ASSERT_THAT(AdbcConnectionRelease(&connection_, &error_), IsOkStatus(&error_));
    }
    if (database_.private_data) {
      ASSERT_THAT(AdbcDatabaseRelease(&database_, &error_), IsOkStatus(&error_));
    }

    if (error_.release) error_.release(&error_);
  }

 protected:
  PostgresQuirks quirks_;
  struct AdbcError error_ = {};
  struct AdbcDatabase database_ = {};
  struct AdbcConnection connection_ = {};
  struct AdbcStatement statement_ = {};
};

static std::optional<std::string> PostgresOnlyTypeCase(const std::string& name) {
  if (name == "MONEY" || name == "SERIAL" || name == "BIGSERIAL") {
    return name + " is PostgreSQL-only";
  }
  if (name == "NUMERIC_NAN" || name == "NUMERIC_NINF" || name == "NUMERIC_PINF") {
    return "DuckDB DECIMAL has no NaN or infinity";
  }
  if (name.size() > 5 && name.rfind("TIME_", 0) == 0 && std::isdigit(name[5])) {
    return "DuckDB TIME takes no precision";
  }
  if (name.rfind("TIMESTAMP_", 0) == 0 && name.size() > 10 && std::isdigit(name[10]) &&
      name.find("_WITH_TIME_ZONE") != std::string::npos) {
    return "DuckDB TIMESTAMP WITH TIME ZONE takes no precision";
  }
  return std::nullopt;
}

TEST_P(PostgresTypeTest, SelectValue) {
  if (auto reason = PostgresOnlyTypeCase(GetParam().name)) {
    GTEST_SKIP() << *reason;
  }
  std::string value = GetParam().sql_literal;
  // create table
  std::string query = "CREATE TABLE foo (col ";
  query += GetParam().sql_type;
  query += ")";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement_, query.c_str(), &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, /*out=*/nullptr,
                                        /*rows_affected=*/nullptr, &error_),
              IsOkStatus(&error_));

  // insert value
  query = "INSERT INTO foo(col) VALUES ( ";
  query += GetParam().sql_literal;
  query += ")";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement_, query.c_str(), &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, /*out=*/nullptr,
                                        /*rows_affected=*/nullptr, &error_),
              IsOkStatus(&error_));

  // select
  adbc_validation::StreamReader reader;
  query = "SELECT * FROM foo";
  ASSERT_THAT(AdbcStatementSetSqlQuery(&statement_, query.c_str(), &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, &reader.stream.value,
                                        /*rows_affected=*/nullptr, &error_),
              IsOkStatus(&error_));

  // check type
  ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
  ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
      &reader.schema.value, {{"", GetParam().arrow_type, true}}));
  if (GetParam().arrow_type == NANOARROW_TYPE_TIMESTAMP) {
    if (GetParam().sql_type.find("WITH TIME ZONE") == std::string::npos) {
      ASSERT_STREQ(reader.schema->children[0]->format, "tsu:");
    } else {
      ASSERT_STREQ(reader.schema->children[0]->format, "tsu:UTC");
    }
  }

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_NE(nullptr, reader.array->release);
  ASSERT_FALSE(ArrowArrayViewIsNull(&reader.array_view.value, 0));
  ASSERT_FALSE(ArrowArrayViewIsNull(reader.array_view->children[0], 0));

  // check value
  ASSERT_NO_FATAL_FAILURE(std::visit(
      [&](auto&& arg) -> void {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, bool>) {
          ASSERT_EQ(static_cast<int64_t>(arg),
                    ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0));
        } else if constexpr (std::is_same_v<T, int64_t>) {
          ASSERT_EQ(arg, ArrowArrayViewGetIntUnsafe(reader.array_view->children[0], 0));
        } else if constexpr (std::is_same_v<T, double>) {
          ASSERT_EQ(arg,
                    ArrowArrayViewGetDoubleUnsafe(reader.array_view->children[0], 0));
        } else if constexpr (std::is_same_v<T, std::string>) {
          ArrowStringView view =
              ArrowArrayViewGetStringUnsafe(reader.array_view->children[0], 0);
          std::string_view v(view.data, static_cast<size_t>(view.size_bytes));
          ASSERT_EQ(arg, v);
        } else if constexpr (std::is_same_v<T, ArrowInterval>) {
          ArrowInterval interval;
          std::memset(&interval, 0, sizeof(interval));
          ArrowArrayViewGetIntervalUnsafe(reader.array_view->children[0], 0, &interval);
          // The getter doesn't set this.
          // EXPECT_EQ(arg.type, interval.type);
          EXPECT_EQ(arg.months, interval.months);
          EXPECT_EQ(arg.days, interval.days);
          EXPECT_EQ(arg.ms, interval.ms);
          EXPECT_EQ(arg.ns, interval.ns);
        } else {
          FAIL() << "Unimplemented case";
        }
      },
      GetParam().scalar));

  ASSERT_NO_FATAL_FAILURE(reader.Next());
  ASSERT_EQ(nullptr, reader.array->release);
}

static std::initializer_list<TypeTestCase> kBoolTypeCases = {
    {"BOOL_TRUE", "BOOLEAN", "TRUE", NANOARROW_TYPE_BOOL, true},
    {"BOOL_FALSE", "BOOLEAN", "FALSE", NANOARROW_TYPE_BOOL, false},
};
static std::initializer_list<TypeTestCase> kBinaryTypeCases = {
    {"BYTEA", "BYTEA", R"('\x00\x01\x02\x03\x04\x05\x06\x07'::bytea)"s,
     NANOARROW_TYPE_BINARY, std::string("\x00\x01\x02\x03\x04\x05\x06\x07", 8)},
    {"TEXT", "TEXT", "'foobar'", NANOARROW_TYPE_STRING, "foobar"s},
    // DuckDB doesn't blank-pad CHAR(n).
    {"CHAR6_1", "CHAR(6)", "'foo'", NANOARROW_TYPE_STRING, "foo"s},
    {"CHAR6_2", "CHAR(6)", "'foobar'", NANOARROW_TYPE_STRING, "foobar"s},
    {"VARCHAR", "VARCHAR", "'foobar'", NANOARROW_TYPE_STRING, "foobar"s},
};
static std::initializer_list<TypeTestCase> kFloatTypeCases = {
    {"REAL", "REAL", "-1E0", NANOARROW_TYPE_FLOAT, -1.0},
    {"DOUBLE_PRECISION", "DOUBLE PRECISION", "-1E0", NANOARROW_TYPE_DOUBLE, -1.0},
};
static std::initializer_list<TypeTestCase> kIntTypeCases = {
    {"SMALLINT", "SMALLINT", std::to_string(std::numeric_limits<int16_t>::min()),
     NANOARROW_TYPE_INT16, static_cast<int64_t>(std::numeric_limits<int16_t>::min())},
    {"INT", "INT", std::to_string(std::numeric_limits<int32_t>::min()),
     NANOARROW_TYPE_INT32, static_cast<int64_t>(std::numeric_limits<int32_t>::min())},
    {"BIGINT", "BIGINT", std::to_string(std::numeric_limits<int64_t>::min()),
     NANOARROW_TYPE_INT64, std::numeric_limits<int64_t>::min()},
    {"SERIAL", "SERIAL", std::to_string(std::numeric_limits<int32_t>::max()),
     NANOARROW_TYPE_INT32, static_cast<int64_t>(std::numeric_limits<int32_t>::max())},
    {"BIGSERIAL", "BIGSERIAL", std::to_string(std::numeric_limits<int64_t>::max()),
     NANOARROW_TYPE_INT64, std::numeric_limits<int64_t>::max()},
};
static std::initializer_list<TypeTestCase> kNumericTypeCases = {
    // DuckDB's unqualified NUMERIC is DECIMAL(18,3).
    {"NUMERIC_TRAILING0", "NUMERIC", "1000000", NANOARROW_TYPE_STRING, "1000000.000"s},
    {"NUMERIC_LEADING0", "NUMERIC(10, 8)", "0.00001234", NANOARROW_TYPE_STRING,
     "0.00001234"s},
    {"NUMERIC_TRAILING02", "NUMERIC(5, 4)", "'1.0000'", NANOARROW_TYPE_STRING, "1.0000"s},
    {"NUMERIC_NEGATIVE", "NUMERIC", "-123.456", NANOARROW_TYPE_STRING, "-123.456"s},
    {"NUMERIC_POSITIVE", "NUMERIC", "123.456", NANOARROW_TYPE_STRING, "123.456"s},
    {"NUMERIC_NAN", "NUMERIC", "'nan'", NANOARROW_TYPE_STRING, "nan"s},
    {"NUMERIC_NINF", "NUMERIC", "'-inf'", NANOARROW_TYPE_STRING, "-inf"s},
    {"NUMERIC_PINF", "NUMERIC", "'inf'", NANOARROW_TYPE_STRING, "inf"s},
    {"MONEY", "MONEY", "12.34", NANOARROW_TYPE_INT64, int64_t(1234)},
};
static std::initializer_list<TypeTestCase> kDateTypeCases = {
    {"DATE0", "DATE", "'1970-01-01'", NANOARROW_TYPE_DATE32, int64_t(0)},
    {"DATE1", "DATE", "'2000-01-01'", NANOARROW_TYPE_DATE32, int64_t(10957)},
    {"DATE2", "DATE", "'1950-01-01'", NANOARROW_TYPE_DATE32, int64_t(-7305)},
};
// DuckDB doesn't parse ISO 8601 intervals and ignores field qualifiers.
static std::initializer_list<TypeTestCase> kIntervalTypeCases = {
    {
        "INTERVAL",
        "INTERVAL",
        "'-1 year 2 months 42 days 01:01:01'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(-10, 42, (1LL * 60 * 60 + 60LL + 1LL) * 1'000'000'000),
    },
    {
        "INTERVAL2",
        "INTERVAL",
        "'0.1 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 100LL * 1'000'000),
    },
    {
        "INTERVAL3",
        "INTERVAL",
        "'0.01 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 10LL * 1'000'000),
    },
    {
        "INTERVAL4",
        "INTERVAL",
        "'0.001 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 1LL * 1'000'000),
    },
    {
        "INTERVAL5",
        "INTERVAL",
        "'0.0001 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 100'000LL),
    },
    {
        "INTERVAL6",
        "INTERVAL",
        "'0.00001 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 10'000LL),
    },
    {
        "INTERVAL7",
        "INTERVAL",
        "'0.000001 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 1'000LL),
    },
    {
        "INTERVAL_YEAR",
        "INTERVAL YEAR",
        "'16Y'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(16 * 12, 0, 0),
    },
    {
        "INTERVAL_MONTH",
        "INTERVAL MONTH",
        "'-2 months'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(-2, 0, 0),
    },
    {
        "INTERVAL_DAY",
        "INTERVAL DAY",
        "'-102D'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, -102, 0),
    },
    {
        "INTERVAL_HOUR",
        "INTERVAL HOUR",
        "'12H'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 12LL * 60 * 60 * 1'000'000'000),
    },
    {
        "INTERVAL_MINUTE",
        "INTERVAL MINUTE",
        "'-5 minutes'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, -5LL * 60 * 1'000'000'000),
    },
    {
        "INTERVAL_SECOND",
        "INTERVAL SECOND",
        "'42 seconds'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 42LL * 1'000'000'000),
    },
    {
        "INTERVAL_YEAR_TO_MONTH",
        "INTERVAL YEAR TO MONTH",
        "'1 year 1 month'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(13, 0, 0),
    },
    {
        "INTERVAL_DAY_TO_HOUR",
        "INTERVAL DAY TO HOUR",
        "'1 day -2 hours'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 1, -2LL * 60 * 60 * 1'000'000'000),
    },
    {
        "INTERVAL_DAY_TO_MINUTE",
        "INTERVAL DAY TO MINUTE",
        "'1 day -2 hours 1 minute'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 1, (-2LL * 60 + 1LL) * 60 * 1'000'000'000),
    },
    {
        "INTERVAL_DAY_TO_SECOND",
        "INTERVAL DAY TO SECOND",
        "'1 day -2 hours 1 minute -1 second'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 1, ((-2LL * 60 + 1LL) * 60 - 1LL) * 1'000'000'000),
    },
    {
        "INTERVAL_HOUR_TO_MINUTE",
        "INTERVAL HOUR TO MINUTE",
        "'-2 hours 1 minute'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, (-2LL * 60 + 1LL) * 60 * 1'000'000'000),
    },
    {
        "INTERVAL_HOUR_TO_SECOND",
        "INTERVAL HOUR TO SECOND",
        "'-2 hours 1 minute -1 second'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, ((-2LL * 60 + 1LL) * 60 - 1LL) * 1'000'000'000),
    },
    {
        "INTERVAL_MINUTE_TO_SECOND",
        "INTERVAL MINUTE TO SECOND",
        "'1 minute -1 second'",
        NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO,
        MonthDayNano(0, 0, 59LL * 1'000'000'000),
    },
};
static std::initializer_list<TypeTestCase> kTimeTypeCases = {
    {"TIME_WITHOUT_TIME_ZONE", "TIME WITHOUT TIME ZONE", "'00:00'", NANOARROW_TYPE_TIME64,
     int64_t(0)},
    {"TIME_WITHOUT_TIME_ZONE_VAL", "TIME WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'123'123)},
    {"TIME_6_WITHOUT_TIME_ZONE", "TIME (6) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_6_WITHOUT_TIME_ZONE_VAL", "TIME (6) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'123'123)},
    {"TIME_5_WITHOUT_TIME_ZONE", "TIME (5) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_5_WITHOUT_TIME_ZONE_VAL", "TIME (5) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'123'120)},
    {"TIME_4_WITHOUT_TIME_ZONE", "TIME (4) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_4_WITHOUT_TIME_ZONE_VAL", "TIME (4) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'123'100)},
    {"TIME_3_WITHOUT_TIME_ZONE", "TIME (3) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_3_WITHOUT_TIME_ZONE_VAL", "TIME (3) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'123'000)},
    {"TIME_2_WITHOUT_TIME_ZONE", "TIME (2) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_2_WITHOUT_TIME_ZONE_VAL", "TIME (2) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'120'000)},
    {"TIME_1_WITHOUT_TIME_ZONE", "TIME (1) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_1_WITHOUT_TIME_ZONE_VAL", "TIME (1) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'100'000)},
    {"TIME_0_WITHOUT_TIME_ZONE", "TIME (0) WITHOUT TIME ZONE", "'00:00'",
     NANOARROW_TYPE_TIME64, int64_t(0)},
    {"TIME_0_WITHOUT_TIME_ZONE_VAL", "TIME (0) WITHOUT TIME ZONE", "'01:02:03.123123'",
     NANOARROW_TYPE_TIME64, int64_t(3'723'000'000)},
};
// DuckDB maps TIMESTAMP (0) to TIMESTAMP_S, (1)-(3) to TIMESTAMP_MS and (4)-(6) to
// TIMESTAMP, truncating rather than rounding.
static std::initializer_list<TypeTestCase> kTimestampTypeCases = {
    {"TIMESTAMP_WITHOUT_TIME_ZONE", "TIMESTAMP WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'123)},
    {"TIMESTAMP_6_WITHOUT_TIME_ZONE", "TIMESTAMP (6) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_6_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (6) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'123)},
    {"TIMESTAMP_5_WITHOUT_TIME_ZONE", "TIMESTAMP (5) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_5_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (5) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'123)},
    {"TIMESTAMP_4_WITHOUT_TIME_ZONE", "TIMESTAMP (4) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_4_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (4) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'123)},
    {"TIMESTAMP_3_WITHOUT_TIME_ZONE", "TIMESTAMP (3) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_3_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (3) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'000)},
    {"TIMESTAMP_2_WITHOUT_TIME_ZONE", "TIMESTAMP (2) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_2_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (2) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'000)},
    {"TIMESTAMP_1_WITHOUT_TIME_ZONE", "TIMESTAMP (1) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_1_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (1) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'123'000)},
    {"TIMESTAMP_0_WITHOUT_TIME_ZONE", "TIMESTAMP (0) WITHOUT TIME ZONE",
     "'1970-01-01 00:00:00.000000'", NANOARROW_TYPE_TIMESTAMP, int64_t(0)},
    {"TIMESTAMP_0_WITHOUT_TIME_ZONE_VAL", "TIMESTAMP (0) WITHOUT TIME ZONE",
     "'1970-01-02 03:04:05.123123'", NANOARROW_TYPE_TIMESTAMP, int64_t(97'445'000'000)},
    {"TIMESTAMP_WITH_TIME_ZONE", "TIMESTAMP WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_WITH_TIME_ZONE_VAL", "TIMESTAMP WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'123'123)},
    {"TIMESTAMP_6_WITH_TIME_ZONE", "TIMESTAMP (6) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_6_WITH_TIME_ZONE_VAL", "TIMESTAMP (6) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'123'123)},
    {"TIMESTAMP_5_WITH_TIME_ZONE", "TIMESTAMP (5) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_5_WITH_TIME_ZONE_VAL", "TIMESTAMP (5) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'123'120)},
    {"TIMESTAMP_4_WITH_TIME_ZONE", "TIMESTAMP (4) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_4_WITH_TIME_ZONE_VAL", "TIMESTAMP (4) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'123'100)},
    {"TIMESTAMP_3_WITH_TIME_ZONE", "TIMESTAMP (3) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_3_WITH_TIME_ZONE_VAL", "TIMESTAMP (3) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'123'000)},
    {"TIMESTAMP_2_WITH_TIME_ZONE", "TIMESTAMP (2) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_2_WITH_TIME_ZONE_VAL", "TIMESTAMP (2) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'120'000)},
    {"TIMESTAMP_1_WITH_TIME_ZONE", "TIMESTAMP (1) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_1_WITH_TIME_ZONE_VAL", "TIMESTAMP (1) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'100'000)},
    {"TIMESTAMP_0_WITH_TIME_ZONE", "TIMESTAMP (0) WITH TIME ZONE",
     "'1970-01-01 00:00:00.000000+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(-1'800'000'000)},
    {"TIMESTAMP_0_WITH_TIME_ZONE_VAL", "TIMESTAMP (0) WITH TIME ZONE",
     "'1970-01-02 03:04:05.123123+00:30'", NANOARROW_TYPE_TIMESTAMP,
     int64_t(95'645'000'000)},
};

INSTANTIATE_TEST_SUITE_P(BoolType, PostgresTypeTest, testing::ValuesIn(kBoolTypeCases),
                         TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(BinaryTypes, PostgresTypeTest,
                         testing::ValuesIn(kBinaryTypeCases), TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(FloatTypes, PostgresTypeTest, testing::ValuesIn(kFloatTypeCases),
                         TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(IntTypes, PostgresTypeTest, testing::ValuesIn(kIntTypeCases),
                         TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(NumericType, PostgresTypeTest,
                         testing::ValuesIn(kNumericTypeCases), TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(DateTypes, PostgresTypeTest, testing::ValuesIn(kDateTypeCases),
                         TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(IntervalTypes, PostgresTypeTest,
                         testing::ValuesIn(kIntervalTypeCases), TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(TimeTypes, PostgresTypeTest, testing::ValuesIn(kTimeTypeCases),
                         TypeTestCase::FormatName);
INSTANTIATE_TEST_SUITE_P(TimestampTypes, PostgresTypeTest,
                         testing::ValuesIn(kTimestampTypeCases),
                         TypeTestCase::FormatName);

struct DecimalTestCase {
  const enum ArrowType type;
  const int32_t precision;
  const int32_t scale;
  const std::vector<std::array<uint64_t, 4>> data;
  const std::vector<std::optional<std::string>> expected;
};

struct DecimalFixtureGroup {
  struct Value {
    std::optional<int64_t> coefficient;
    std::optional<std::string> expected;
  };

  int32_t precision;
  int32_t scale;
  std::vector<Value> values;
};

static DecimalFixtureGroup::Value DecimalCoefficientValue(
    int64_t coefficient, std::optional<std::string> expected) {
  return {coefficient, std::move(expected)};
}

static void AppendDecimalFixtureValue(std::vector<DecimalFixtureGroup>* groups,
                                      int32_t precision, int32_t scale,
                                      DecimalFixtureGroup::Value value) {
  if (groups->empty() || groups->back().precision != precision ||
      groups->back().scale != scale) {
    groups->push_back({precision, scale, {}});
  }

  groups->back().values.push_back(std::move(value));
}

static std::vector<DecimalFixtureGroup> LoadGeneratedDecimalFixtureGroups() {
  std::ifstream input(ADBC_POSTGRESQL_TESTDATA_DIR "/numeric_roundtrip_fixtures.csv");
  if (!input.is_open()) {
    ADD_FAILURE() << "Failed to open numeric fixture file at "
                  << ADBC_POSTGRESQL_TESTDATA_DIR << "/numeric_roundtrip_fixtures.csv";
    return {};
  }

  std::string line;
  if (!std::getline(input, line)) {
    ADD_FAILURE() << "Numeric fixture file is empty";
    return {};
  }

  std::vector<DecimalFixtureGroup> groups;
  while (std::getline(input, line)) {
    if (line.empty()) continue;

    std::stringstream line_stream(line);
    std::string precision_str;
    std::string scale_str;
    std::string coefficient_str;
    std::string expected_str;

    if (!std::getline(line_stream, precision_str, ',') ||
        !std::getline(line_stream, scale_str, ',') ||
        !std::getline(line_stream, coefficient_str, ',') ||
        !std::getline(line_stream, expected_str)) {
      ADD_FAILURE() << "Malformed numeric fixture row: " << line;
      return {};
    }

    const int32_t precision = std::stoi(precision_str);
    const int32_t scale = std::stoi(scale_str);
    const int64_t coefficient = std::stoll(coefficient_str);

    AppendDecimalFixtureValue(&groups, precision, scale,
                              DecimalCoefficientValue(coefficient, expected_str));
  }

  return groups;
}

static const std::vector<DecimalFixtureGroup>& DecimalFixtureGroups() {
  static const std::vector<DecimalFixtureGroup> groups =
      LoadGeneratedDecimalFixtureGroups();
  return groups;
}

class PostgresDecimalTestBase : public ::testing::Test {
 public:
  void SetUp() override {
    SKIP_WITHOUT_COPY_FROM_STDIN();
    ASSERT_THAT(AdbcDatabaseNew(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(quirks_.SetupDatabase(&database_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcDatabaseInit(&database_, &error_), IsOkStatus(&error_));

    ASSERT_THAT(AdbcConnectionNew(&connection_, &error_), IsOkStatus(&error_));
    ASSERT_THAT(AdbcConnectionInit(&connection_, &database_, &error_),
                IsOkStatus(&error_));

    ASSERT_THAT(AdbcStatementNew(&connection_, &statement_, &error_),
                IsOkStatus(&error_));

    ASSERT_THAT(quirks_.DropTable(&connection_, "bulk_ingest", &error_),
                IsOkStatus(&error_));
  }

  void TearDown() override {
    if (statement_.private_data) {
      ASSERT_THAT(AdbcStatementRelease(&statement_, &error_), IsOkStatus(&error_));
    }
    if (connection_.private_data) {
      ASSERT_THAT(AdbcConnectionRelease(&connection_, &error_), IsOkStatus(&error_));
    }
    if (database_.private_data) {
      ASSERT_THAT(AdbcDatabaseRelease(&database_, &error_), IsOkStatus(&error_));
    }

    if (error_.release) error_.release(&error_);
  }

 protected:
  PostgresQuirks quirks_;
  struct AdbcError error_ = {};
  struct AdbcDatabase database_ = {};
  struct AdbcConnection connection_ = {};
  struct AdbcStatement statement_ = {};
};

class PostgresDecimalTest : public PostgresDecimalTestBase,
                            public ::testing::WithParamInterface<DecimalTestCase> {};

class PostgresDecimalFixtureRoundTripTest : public PostgresDecimalTestBase,
                                            public ::testing::WithParamInterface<bool> {};

TEST_P(PostgresDecimalTest, SelectValue) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> array;
  struct ArrowError na_error;

  const enum ArrowType type = GetParam().type;
  const int32_t precision = GetParam().precision;
  if (precision > 38) {
    GTEST_SKIP() << "DuckDB DECIMAL precision is at most 38";
  }
  const int32_t scale = GetParam().scale;
  const auto data = GetParam().data;
  const auto expected = GetParam().expected;
  const size_t nrecords = expected.size();

  int32_t bitwidth;
  switch (type) {
    case NANOARROW_TYPE_DECIMAL128:
      bitwidth = 128;
      break;
    case NANOARROW_TYPE_DECIMAL256:
      bitwidth = 256;
      break;
    default:
      FAIL();
  }

  // this is a bit of a hack to make std::vector play nicely with
  // a dynamic number of stack-allocated ArrowDecimal objects
  constexpr size_t max_decimals = 10;
  struct ArrowDecimal decimals[max_decimals];
  if (nrecords > max_decimals) {
    FAIL() << " max_decimals exceeded for test case - please change parametrization";
  }

  std::vector<std::optional<ArrowDecimal*>> values;
  for (size_t i = 0; i < nrecords; i++) {
    ArrowDecimalInit(&decimals[i], bitwidth, precision, scale);
    uint8_t buf[32];
    const auto record = data[i];
    memcpy(buf, record.data(), sizeof(buf));
    ArrowDecimalSetBytes(&decimals[i], buf);
    values.push_back(&decimals[i]);
  }

  auto expected_with_null{expected};
  expected_with_null.insert(expected_with_null.begin(), std::nullopt);
  values.push_back(std::nullopt);

  ArrowSchemaInit(&schema.value);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(&schema.value, 1), 0);
  ASSERT_EQ(ArrowSchemaSetTypeDecimal(schema.value.children[0], type, precision, scale),
            0);
  ASSERT_EQ(ArrowSchemaSetName(schema.value.children[0], "col"), 0);

  ASSERT_THAT(adbc_validation::MakeBatch<ArrowDecimal*>(&schema.value, &array.value,
                                                        &na_error, values),
              adbc_validation::IsOkErrno());

  ASSERT_THAT(AdbcStatementSetOption(&statement_, ADBC_INGEST_OPTION_TARGET_TABLE,
                                     "bulk_ingest", &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(AdbcStatementBind(&statement_, &array.value, &schema.value, &error_),
              IsOkStatus(&error_));

  int64_t rows_affected = 0;
  ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, nullptr, &rows_affected, &error_),
              IsOkStatus(&error_));
  ASSERT_THAT(rows_affected,
              ::testing::AnyOf(::testing::Eq(values.size()), ::testing::Eq(-1)));

  ASSERT_THAT(AdbcStatementSetSqlQuery(
                  &statement_,
                  "SELECT * FROM bulk_ingest ORDER BY \"col\" ASC NULLS FIRST", &error_),
              IsOkStatus(&error_));

  {
    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, &reader.stream.value,
                                          &reader.rows_affected, &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(reader.rows_affected,
                ::testing::AnyOf(::testing::Eq(values.size()), ::testing::Eq(-1)));

    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ArrowType round_trip_type = quirks_.IngestSelectRoundTripType(type);
    ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareSchema(
        &reader.schema.value, {{"col", round_trip_type, true}}));

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);
    ASSERT_EQ(values.size(), reader.array->length);
    ASSERT_EQ(1, reader.array->n_children);

    ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
        reader.array_view->children[0], expected_with_null));

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(nullptr, reader.array->release);
  }
}

TEST_P(PostgresDecimalFixtureRoundTripTest, Decimal128FixtureRoundTrip) {
  const bool disable_decimal_fast_path = GetParam();
  const char* fast_path_value =
      disable_decimal_fast_path ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;

  ASSERT_THAT(
      AdbcStatementSetOption(&statement_, "adbc.postgresql.disable_decimal_fast_path",
                             fast_path_value, &error_),
      IsOkStatus(&error_));

  const auto& groups = DecimalFixtureGroups();
  ASSERT_FALSE(groups.empty());

  auto run_group = [&](const DecimalFixtureGroup& group) {
    SCOPED_TRACE("precision=" + std::to_string(group.precision) +
                 " scale=" + std::to_string(group.scale) + " disable_decimal_fast_path=" +
                 std::string(disable_decimal_fast_path ? "true" : "false"));

    ASSERT_THAT(quirks_.DropTable(&connection_, "bulk_ingest", &error_),
                IsOkStatus(&error_));

    adbc_validation::Handle<struct ArrowSchema> schema;
    adbc_validation::Handle<struct ArrowArray> array;
    struct ArrowError na_error;

    std::vector<ArrowDecimal> decimals(group.values.size());
    std::vector<std::optional<ArrowDecimal*>> values;
    std::vector<std::optional<std::string>> expected;
    values.reserve(group.values.size());
    expected.reserve(group.values.size());
    for (size_t i = 0; i < group.values.size(); i++) {
      const auto& value = group.values[i];
      expected.push_back(value.expected);
      if (!value.coefficient.has_value()) {
        values.push_back(std::nullopt);
        continue;
      }

      ArrowDecimalInit(&decimals[i], /*bitwidth=*/128, group.precision, group.scale);
      ArrowDecimalSetInt(&decimals[i], *value.coefficient);
      values.push_back(&decimals[i]);
    }

    ArrowSchemaInit(&schema.value);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(&schema.value, 1), 0);
    ASSERT_EQ(
        ArrowSchemaSetTypeDecimal(schema.value.children[0], NANOARROW_TYPE_DECIMAL128,
                                  group.precision, group.scale),
        0);
    ASSERT_EQ(ArrowSchemaSetName(schema.value.children[0], "amount"), 0);
    ASSERT_THAT(adbc_validation::MakeBatch<ArrowDecimal*>(&schema.value, &array.value,
                                                          &na_error, values),
                adbc_validation::IsOkErrno());

    ASSERT_THAT(AdbcStatementSetOption(&statement_, ADBC_INGEST_OPTION_TARGET_TABLE,
                                       "bulk_ingest", &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(AdbcStatementBind(&statement_, &array.value, &schema.value, &error_),
                IsOkStatus(&error_));

    int64_t rows_affected = 0;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, nullptr, &rows_affected, &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(rows_affected,
                ::testing::AnyOf(::testing::Eq(static_cast<int64_t>(values.size())),
                                 ::testing::Eq(-1)));

    ASSERT_THAT(
        AdbcStatementSetSqlQuery(
            &statement_, "SELECT amount::text AS amount FROM bulk_ingest ORDER BY ctid",
            &error_),
        IsOkStatus(&error_));

    adbc_validation::StreamReader reader;
    ASSERT_THAT(AdbcStatementExecuteQuery(&statement_, &reader.stream.value,
                                          &reader.rows_affected, &error_),
                IsOkStatus(&error_));
    ASSERT_THAT(reader.rows_affected,
                ::testing::AnyOf(::testing::Eq(static_cast<int64_t>(values.size())),
                                 ::testing::Eq(-1)));

    ASSERT_NO_FATAL_FAILURE(reader.GetSchema());
    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_NE(nullptr, reader.array->release);
    ASSERT_EQ(static_cast<int64_t>(values.size()), reader.array->length);
    ASSERT_EQ(1, reader.array->n_children);

    ASSERT_NO_FATAL_FAILURE(adbc_validation::CompareArray<std::string>(
        reader.array_view->children[0], expected));

    ASSERT_NO_FATAL_FAILURE(reader.Next());
    ASSERT_EQ(nullptr, reader.array->release);
  };

  for (const auto& group : groups) {
    run_group(group);
  }
}

static std::vector<std::array<uint64_t, 4>> kDecimalData = {
    // -12345600000
    {18446744061363951616ULL, 18446744073709551615ULL, 0, 0},
    // 1234
    {1234ULL, 0, 0, 0},
    // 100000000
    {100000000ULL, 0, 0, 0},
    // 12345600000
    {12345600000ULL, 0, 0, 0},
    // 100000000000000
    {100000000000000ULL, 0, 0, 0},
    // 2342394230592232349023094
    {8221368519775271798ULL, 126981ULL, 0, 0},
};

static std::vector<std::array<uint64_t, 4>> kDecimal256Data = {
    // 1234567890123456789012345678901234567890123456789012345678901234567890123456
    {17877984925544397504ULL, 5352188884907840935ULL, 234631617561833724ULL,
     196678011949953713ULL},
    // -1234567890123456789012345678901234567890123456789012345678901234567890123456
    {568759148165154112ULL, 13094555188801710680ULL, 18212112456147717891ULL,
     18250066061759597902ULL},
};

static std::initializer_list<DecimalTestCase> kDecimal128Cases = {
    {NANOARROW_TYPE_DECIMAL128,
     38,
     8,
     kDecimalData,
     {"-123.456", "0.00001234", "1", "123.456", "1000000",
      "23423942305922323.49023094"}}};

static std::initializer_list<DecimalTestCase> kDecimal128NoScaleCases = {
    {NANOARROW_TYPE_DECIMAL128,
     38,
     0,
     kDecimalData,
     {"-12345600000", "1234", "100000000", "12345600000", "100000000000000",
      "2342394230592232349023094"}}};

static std::initializer_list<DecimalTestCase> kDecimal256Cases = {
    {NANOARROW_TYPE_DECIMAL256,
     38,
     8,
     kDecimalData,
     {"-123.456", "0.00001234", "1", "123.456", "1000000",
      "23423942305922323.49023094"}}};

static std::initializer_list<DecimalTestCase> kDecimal256NoScaleCases = {
    {NANOARROW_TYPE_DECIMAL256,
     38,
     0,
     kDecimalData,
     {"-12345600000", "1234", "100000000", "12345600000", "100000000000000",
      "2342394230592232349023094"}}};

static std::initializer_list<DecimalTestCase> kDecimal256LargeCases = {
    {NANOARROW_TYPE_DECIMAL256,
     76,
     8,
     kDecimal256Data,
     {
         "-12345678901234567890123456789012345678901234567890123456789012345678.90123456",
         "12345678901234567890123456789012345678901234567890123456789012345678.90123456",
     }}};

static std::initializer_list<DecimalTestCase> kDecimal256LargeNoScaleCases = {
    {NANOARROW_TYPE_DECIMAL256,
     76,
     0,
     kDecimal256Data,
     {
         "-1234567890123456789012345678901234567890123456789012345678901234567890123456",
         "1234567890123456789012345678901234567890123456789012345678901234567890123456",
     }},
    {NANOARROW_TYPE_DECIMAL256,
     76,
     0,
     {
         // -18446744073709551617, -18446744073709551616, -18446744073709551615
         {UINT64_MAX, UINT64_MAX - 1, UINT64_MAX, UINT64_MAX},
         {0, UINT64_MAX, UINT64_MAX, UINT64_MAX},
         {1, UINT64_MAX, UINT64_MAX, UINT64_MAX},
     },
     {"-18446744073709551617", "-18446744073709551616", "-18446744073709551615"}}};

INSTANTIATE_TEST_SUITE_P(Decimal128Tests, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal128Cases));
INSTANTIATE_TEST_SUITE_P(Decimal128NoScale, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal128NoScaleCases));
INSTANTIATE_TEST_SUITE_P(Decimal256Tests, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal128Cases));
INSTANTIATE_TEST_SUITE_P(Decimal256NoScale, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal128NoScaleCases));
INSTANTIATE_TEST_SUITE_P(Decimal256LargeTests, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal256LargeCases));
INSTANTIATE_TEST_SUITE_P(Decimal256LargeNoScale, PostgresDecimalTest,
                         testing::ValuesIn(kDecimal256LargeNoScaleCases));
INSTANTIATE_TEST_SUITE_P(Decimal128FixtureRoundTrip, PostgresDecimalFixtureRoundTripTest,
                         testing::Bool(), [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? "SlowPath" : "FastPath";
                         });
