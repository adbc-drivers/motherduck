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

// Tests for masking credentials in SQL text that appears in error messages.

#include <cstdlib>
#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <libpq-fe.h>

#include "error.h"
#include "motherduck_redact.h"

namespace adbcpq {

TEST(MotherDuckRedactTest, ExactOutputs) {
  const std::pair<const char*, const char*> kCases[] = {
      {"CREATE SECRET s (TYPE s3, KEY_ID 'k', SECRET 'x')", "CREATE SECRET s (...)"},
      {"create or replace persistent secret s IN motherduck (TYPE s3, SECRET 'x')",
       "create or replace persistent secret s IN motherduck (...)"},
      {"CREATE TEMP SECRET s (SECRET 'x')", "CREATE TEMP SECRET s (...)"},
      {"ALTER SECRET s (SECRET 'x')", "ALTER SECRET s (...)"},
      {"SET s3_secret_access_key = 'x'", "SET s3_secret_access_key = ..."},
      {"SET GLOBAL motherduck_token TO 'x'", "SET GLOBAL motherduck_token TO ..."},
      {"SET memory.s3_secret_access_key = 'x'", "SET memory.s3_secret_access_key = ..."},
      {"ATTACH 'md:db?motherduck_token=abc&x=1'", "ATTACH 'md:db?motherduck_token= ..."},
      {"ATTACH 'postgresql://user:abc@host/db'", "ATTACH 'postgresql://user:..."},
      {"ATTACH 'host=h' AS p (TYPE postgres, PASSWORD 'abc')",
       "ATTACH 'host=h' AS p (TYPE postgres, PASSWORD ..."},
  };
  for (const auto& [query, expected] : kCases) {
    EXPECT_EQ(RedactSecretStatements(query), expected) << query;
  }
}

// None of these may let the marker through, however the statement is dressed up.
TEST(MotherDuckRedactTest, NeverLeaksTheSecret) {
  const char* kQueries[] = {
      "CREATE /* see (note) */ SECRET s (TYPE s3, SECRET 'ZZ9')",
      "CREATE -- (c)\n SECRET s (SECRET 'ZZ9')",
      "CREATE SECRET s SECRET 'ZZ9' -- (forgot parens)",
      "CREATE SECRET s SECRET 'ZZ9' FROM read_parquet('s3://b')",
      "CREATE SECRET s -- (note)\n (SECRET 'ZZ9')",
      "CREATE OR REPLACE TEMPORARY SECRET IF NOT EXISTS s IN motherduck (SECRET 'ZZ9')",
      "SELECT 1; CREATE SECRET s (SECRET 'ZZ9')",
      "SELECT $foo bar$ CREATE SECRET s (SECRET 'ZZ9')",
      "CREATE MACRO m() AS $$ CREATE SECRET s (SECRET 'ZZ9') $$",
      "-- CREATE SECRET s (SECRET 'ZZ9')\nSELECT 1",
      "-- motherduck_token = ZZ9\nSELECT 1",
      "/* token=\"ZZ9\" */ SELECT 1",
      "/* password=ZZ9 */ SELECT 1",
      "SET \"s3_secret_access_key\" = 'ZZ9'",
      "SET 's3_secret_access_key' = 'ZZ9'",
      "SET s3_secret_access_key\n  = 'ZZ9'",
      "SET s3_secret_access_key /* c */ = 'ZZ9'",
      "SET motherduck_token -- c\n TO 'ZZ9'",
      "SET s3_access_key_id = 'ZZ9'",
      "ATTACH 'md:db?motherduck_token=ZZ9&x=1'",
      "ATTACH 'md:db?motherduck_token = ZZ9'",
      "ATTACH 'md:db?motherduck_token=''ZZ9'''",
      "ATTACH 'md:db?motherduck_token=my ZZ9 x'",
      "ATTACH 'md:db?motherduck_token=abc;ZZ9'",
      "ATTACH E'md:db?motherduck_token=abc\\'ZZ9'",
      "SELECT 's3://b/o?access_key_id=ZZ9&secret_access_key=ZZ9'",
      "SELECT \"md:db?token='ZZ9'\"",
      "ATTACH 'postgresql://user:ZZ9@host/db'",
      "SELECT 's3://AKIA:ZZ9@bucket/path'",
      "ATTACH 'host=h' AS p (TYPE postgres, PASSWORD 'ZZ9')",
      "SELECT '{\"password\": \"ZZ9\"}'",
      // DuckDB: Parquet and database file encryption.
      "PRAGMA add_parquet_key('key128', 'ZZ9')",
      "CALL add_parquet_key('key128', 'ZZ9')",
      "ATTACH 'enc.db' AS e (ENCRYPTION_KEY 'ZZ9')",
      // Scanner connection strings.
      "ATTACH 'host=h user=u password=ZZ9 dbname=d' AS p (TYPE postgres)",
      "ATTACH 'host=h user=u passwd=ZZ9' AS m (TYPE mysql)",
      "SELECT * FROM postgres_scan('host=h password=ZZ9', 'public', 't')",
      "ATTACH 'host=h sslkey=/k.pem sslpassword=ZZ9' AS p (TYPE postgres)",
      "ATTACH 'ducklake:postgres:host=h password=ZZ9' AS dl (DATA_PATH 's3://b/')",
      // httpfs and Azure.
      "SET s3_session_token = 'ZZ9'",
      "SET http_proxy_password = 'ZZ9'",
      "SET hf_token = 'ZZ9'",
      "SET azure_storage_connection_string = 'AccountName=a;AccountKey=ZZ9'",
      "SET VARIABLE pw = 'ZZ9'",
      "CREATE USER u WITH PASSWORD 'ZZ9'",
      // Credentials in URLs.
      "SELECT * FROM read_parquet('https://a.blob.core.windows.net/f?sv=1&sig=ZZ9')",
      "SELECT * FROM read_parquet('https://b.s3.amazonaws.com/f?X-Amz-Signature=ZZ9')",
      "SELECT * FROM read_csv('https://x/f?apikey=ZZ9')",
      "SELECT * FROM read_parquet('azure://c/f?AccountKey=ZZ9')",
      "INSTALL x FROM 'https://u:ZZ9@repo.example.com'",
      // MotherDuck connection strings.
      "ATTACH 'md:ZZ9@mydb'",
      "ATTACH 'md:ZZ9@mydb;attach_mode=single'",
      "ATTACH 'motherduck:ZZ9@mydb'",
      "ATTACH 'md:mydb?slt=ZZ9'",
      "SET motherduck_slt = 'ZZ9'",
      "SET motherduck_oauth_token = 'ZZ9'",
      "PRAGMA md_connect('slt=ZZ9')",
      "PRAGMA md_connect('token=ZZ9;host=h')",
      // MotherDuck Flights carry free-form payloads.
      "CALL md_create_flight(name := 'f', source_code := 'ZZ9')",
      "CALL md_run_flight(id := 'x', config := MAP {'A': 'ZZ9'})",
      // HTTP headers.
      "SELECT http_get('https://x', headers = {'Authorization': 'Bearer ZZ9'})",
  };
  for (const char* query : kQueries) {
    const std::string out = RedactSecretStatements(query);
    EXPECT_EQ(out.find("ZZ9"), std::string::npos) << query << "\n -> " << out;
  }
}

TEST(MotherDuckRedactTest, OrdinaryQueriesAreUnchanged) {
  const char* kQueries[] = {
      "SELECT 1",
      "SELECT keyboard, key FROM t ORDER BY id",
      "SELECT key::int FROM t",
      "ATTACH 'md:my_db' AS d (READ_ONLY)",
      "SELECT * FROM md_list_flights()",
      "CREATE TABLE t (id INT, PRIMARY KEY (id))",
      "CREATE TABLE t (id INT PRIMARY KEY, a INT, FOREIGN KEY (a) REFERENCES u(id))",
      "SELECT author, designation FROM t",
      "SELECT * FROM duckdb_secrets()",
      "DROP SECRET s",
      "CREATE TABLE secrets (id INT)",
      "SELECT 'token' AS kind FROM t",
      "SELECT count(*) FROM t GROUP BY key ORDER BY key",
      "SELECT a, b FROM t WHERE monkey > 1 AND tokenizer IS NULL",
      "SET threads = 4",
      "SET memory_limit = '1GB'",
      "COPY t TO 'x.csv' (FORMAT csv)",
      "INSERT INTO t VALUES ('password reset requested')",
  };
  for (const char* query : kQueries) {
    EXPECT_EQ(RedactSecretStatements(query), query);
  }
}

// The scan trades precision for safety. These are masked although they hold no secret;
// pinning them keeps the trade-off visible.
TEST(MotherDuckRedactTest, OverMasksCredentialLookingSql) {
  EXPECT_EQ(RedactSecretStatements("SELECT * FROM t WHERE key = 5 ORDER BY id"),
            "SELECT * FROM t WHERE key = ...");
  EXPECT_EQ(RedactSecretStatements("CREATE TABLE t (secret INT, d DECIMAL(10,2))"),
            "CREATE TABLE t (secret INT, d DECIMAL(...)");
  // Run-together names are matched by their ending.
  EXPECT_EQ(RedactSecretStatements("SELECT * FROM t WHERE monkey = 1"),
            "SELECT * FROM t WHERE monkey = ...");
}

TEST(MotherDuckRedactTest, MalformedInputDoesNotCrash) {
  for (const char* query :
       {"", "'", "\"", "/*", "--", "E'\\", "$$", "$a$", "key", "key '",
        "key =", "token TO", "://", "x://u:", "CREATE SECRET", "secret ("}) {
    RedactSecretStatements(query);
  }
}

TEST(MotherDuckRedactTest, InstalledRedactorAppliesToErrors) {
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET s (SECRET 'x')");
  InstallMotherDuckQueryRedactor();
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET s (...)");
  SetQueryRedactor(nullptr);
}

// libpq echoes the statement ("LINE 1: ...") in its default error message. Make sure the
// text we put in errors never has it.
TEST(MotherDuckRedactTest, ErrorTextOmitsStatementEcho) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  if (!uri) GTEST_SKIP() << "ADBC_POSTGRESQL_TEST_URI not set";
  PGconn* conn = PQconnectdb(uri);
  ASSERT_EQ(PQstatus(conn), CONNECTION_OK) << PQerrorMessage(conn);
  PGresult* result = PQexec(conn, "SELECT FROM WHERE 'UNIQUEMARKER' +");
  ASSERT_EQ(PQresultStatus(result), PGRES_FATAL_ERROR);
  ASSERT_NE(std::string(PQresultErrorMessage(result)).find("UNIQUEMARKER"),
            std::string::npos)
      << "server did not echo the statement; test is vacuous";
  const std::string text = PqErrorText(result, conn);
  EXPECT_EQ(text.find("UNIQUEMARKER"), std::string::npos) << text;
  EXPECT_EQ(text.find("LINE"), std::string::npos) << text;
  PQclear(result);
  PQfinish(conn);
}

}  // namespace adbcpq
