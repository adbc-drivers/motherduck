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
      {"CREATE SECRET s (TYPE s3, KEY_ID 'k', SECRET 'x')", "CREATE SECRET ..."},
      {"create or replace persistent secret s IN motherduck (TYPE s3, SECRET 'x')",
       "create or replace persistent secret ..."},
      {"CREATE TEMP SECRET s (SECRET 'x')", "CREATE TEMP SECRET ..."},
      {"ALTER SECRET s (SECRET 'x')", "ALTER SECRET ..."},
      {"SET s3_secret_access_key = 'x'", "SET s3_secret_access_key ..."},
      {"SET GLOBAL motherduck_token TO 'x'", "SET GLOBAL motherduck_token ..."},
      {"SET memory.s3_secret_access_key = 'x'", "SET memory.s3_secret_access_key ..."},
      {"ATTACH 'md:db?motherduck_token=abc&x=1'", "ATTACH 'md:db?motherduck_token ..."},
      {"ATTACH 'postgresql://user:abc@host/db'", "ATTACH 'postgresql://user: ..."},
      {"ATTACH 'host=h' AS p (TYPE postgres, PASSWORD 'abc')",
       "ATTACH 'host=h' AS p (TYPE postgres, PASSWORD ..."},
  };
  for (const auto& [query, expected] : kCases) {
    EXPECT_EQ(RedactSecretStatements(query), expected) << query;
  }
}

// None of these may let the marker through, however the statement is dressed up.
TEST(MotherDuckRedactTest, NeverLeaksTheSecret) {
  const std::string kQueries[] = {
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
      "SET t.\"s3_secret_access_key\" = 'ZZ9'",
      // A quote that ends the previous literal is not part of a quoted name.
      "CREATE USER 'u'PASSWORD'ZZ9'",
      "CREATE USER ''PASSWORD'ZZ9'",
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
      "ATTACH 'postgresql://user: ZZ9@host/db'",
      "ATTACH 'postgresql://user:\tZZ9@host/db'",
      "ATTACH 'postgresql://user:\n  ZZ9@host/db'",
      "SELECT 's3://AKIA:ZZ9@bucket/path'",
      "ATTACH 'host=h' AS p (TYPE postgres, PASSWORD 'ZZ9')",
      "SELECT '{\"password\": \"ZZ9\"}'",
      // DuckDB: Parquet and database file encryption.
      "PRAGMA add_parquet_key('key128', 'ZZ9')",
      "SET key = 'ZZ9'",
      "SELECT '{\"key\": \"ZZ9\"}'",
      "COPY t TO 'f' (KEY 'ZZ9')",
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
      "ATTACH 'host=h user=u pass=ZZ9' AS m (TYPE mysql)",
      "SET db_pass = 'ZZ9'",
      // Column lists: the values come later.
      "INSERT INTO creds (id, password) VALUES (1, 'ZZ9')",
      "INSERT INTO creds (api_key, id) VALUES ('ZZ9', 1)",
      "SET http_bearer = 'ZZ9'",
      "SET session_jwt = 'ZZ9'",
      "SET sticky_cookie = 'ZZ9'",
      "ATTACH 'dsn=ZZ9' AS m (TYPE odbc)",
      "CREATE USER u WITH PASSWORD 'ZZ9'",
      // A string glued to the keyword, or double-quoted, is still the value.
      "CREATE USER u WITH PASSWORD'ZZ9'",
      "CREATE USER u WITH PASSWORD \"ZZ9\"",
      "SET \"s3_secret_access_key\" = 'ZZ9'",
      // An unclosed comment doesn't hide what follows it.
      "SET s3_secret_access_key /* = 'ZZ9'",
      "SET s3_secret_access_key -- = 'ZZ9'",
      "CALL md_create_flight(source_code /* := 'ZZ9'",
      // Credentials in URLs.
      "SELECT * FROM read_parquet('https://a.blob.core.windows.net/f?sv=1&sig=ZZ9')",
      "SELECT * FROM read_parquet('https://b.s3.amazonaws.com/f?X-Amz-Signature=ZZ9')",
      "SELECT * FROM read_csv('https://x/f?apikey=ZZ9')",
      "SELECT * FROM read_parquet('azure://c/f?AccountKey=ZZ9')",
      "INSTALL x FROM 'https://u:ZZ9@repo.example.com'",
      // MotherDuck connection strings.
      "ATTACH 'md:ZZ9@mydb'",
      "ATTACH 'md:ZZ9@mydb;attach_mode=single'",
      "ATTACH 'md: ZZ9@mydb'",
      // Tokens are JWTs, far longer than a password.
      "ATTACH 'md:ZZ9" + std::string(900, 'x') + "@mydb'",
      "INSTALL x FROM 'https://u:ZZ9" + std::string(900, 'x') + "@repo.example.com'",
      "ATTACH 'md:/* c */ZZ9@mydb'",
      "ATTACH 'motherduck:ZZ9@mydb'",
      "ATTACH 'md:mydb?slt=ZZ9'",
      "SET motherduck_slt = 'ZZ9'",
      "SET motherduck_oauth_token = 'ZZ9'",
      "PRAGMA md_connect('slt=ZZ9')",
      "PRAGMA md_connect('token=ZZ9;host=h')",
      // MotherDuck Flights carry free-form payloads.
      "CALL md_create_flight(name := 'f', source_code := 'ZZ9')",
      "CALL md_create_flight(name => 'f', source_code => 'ZZ9')",
      "SELECT * FROM md_create_flight(name = 'f', source_code = 'x = \"ZZ9\"')",
      "CALL md_run_flight(id => 'x', config => MAP {'A': 'ZZ9'})",
      // Postgres-style unique violation detail echoes the column value.
      "ERROR: duplicate key value violates unique constraint \"t_pkey\"\nDETAIL: Key "
      "(token)=(ZZ9) already exists.",
      "DETAIL: Key (id, api_key)=(1, ZZ9) already exists.",
      "CALL md_run_flight(id := 'x', config := MAP {'A': 'ZZ9'})",
      // HTTP headers.
      "SELECT http_get('https://x', headers = {'Authorization': 'Bearer ZZ9'})",
  };
  for (const std::string& query : kQueries) {
    const std::string out = RedactSecretStatements(query);
    EXPECT_EQ(out.find("ZZ9"), std::string::npos) << query << "\n -> " << out;
  }
}

TEST(MotherDuckRedactTest, OrdinaryQueriesAreUnchanged) {
  const char* kQueries[] = {
      "SELECT 1",
      "SELECT key::int FROM t",
      "SELECT config, source_code FROM flights WHERE config IS NOT NULL",
      "ATTACH 'md:my_db' AS d (READ_ONLY)",
      "SELECT * FROM md_list_flights()",
      "SELECT author, designation FROM t",
      "DROP SECRET s",
      "SELECT count(*) FROM t GROUP BY key ORDER BY key",
      "SELECT keyboard, key FROM t ORDER BY id",
      "CREATE TABLE t (id INT, PRIMARY KEY (id))",
      "Constraint Error: Duplicate key \"id: 1\" violates primary key constraint",
      "DETAIL: Key (id)=(1) already exists.",
      // Server messages that quote a credential-looking name keep their reason.
      "Binder Error: Referenced column \"api_key\" not found in FROM clause!",
      "Catalog Error: Table with name \"tokens\" does not exist!",
      "SELECT \"password\", a FROM t",
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

// Masked although they hold no secret; pinned to keep the trade-off visible.
TEST(MotherDuckRedactTest, OverMasksCredentialLookingSql) {
  const std::pair<const char*, const char*> kCases[] = {
      {"SELECT * FROM t WHERE key = 5 ORDER BY id", "SELECT * FROM t WHERE key ..."},
      {"SELECT * FROM t WHERE monkey = 1", "SELECT * FROM t WHERE monkey ..."},
      {"SELECT * FROM duckdb_secrets()", "SELECT * FROM duckdb_secrets ..."},
      {"CREATE TABLE secrets (id INT)", "CREATE TABLE secrets ..."},
      {"UPDATE settings SET config = 'x' WHERE id = 1", "UPDATE settings SET config ..."},
      {"WHERE kind = 'password' AND x = 'y'", "WHERE kind = 'password ..."},
  };
  for (const auto& [query, expected] : kCases) {
    EXPECT_EQ(RedactSecretStatements(query), expected) << query;
  }
}

TEST(MotherDuckRedactTest, MalformedInputDoesNotCrash) {
  for (const char* query :
       {"", "'", "\"", "/*", "--", "E'\\", "$$", "$a$", "key", "key '",
        "key =", "token TO", "://", "x://u:", "CREATE SECRET", "secret ("}) {
    RedactSecretStatements(query);
  }
  // The largest input the driver hands over (server text), full of triggers.
  for (const char* unit : {"md:", "://a:", "key ", "create ", "/*", "'", "secret "}) {
    std::string text;
    while (text.size() < kMaxMessageBytes) text += unit;
    RedactSecretStatements(text);
  }
}

TEST(MotherDuckRedactTest, InstalledRedactorAppliesToErrors) {
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET s (SECRET 'x')");
  InstallMotherDuckQueryRedactor();
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET ...");
  SetQueryRedactor(nullptr);
}

// Text cut anywhere inside a secret must not leak the part that is left, including a
// URL or md: token cut before its @.
TEST(MotherDuckRedactTest, EveryPrefixOfAStatementHidesTheSecret) {
  for (const char* query :
       {"SELECT 1 FROM t WITH PASSWORD 'ZZZZZZZZ'",
        "ATTACH 'postgresql://user:ZZZZZZZZ@host/db'", "ATTACH 'md:ZZZZZZZZ@db'",
        "SET s3_secret_access_key = 'ZZZZZZZZ'", "ATTACH 'host=h password=ZZZZZZZZ'",
        "COPY t TO 'f' (KEY 'ZZZZZZZZ')", "SET key = ZZZZZZZZ",
        "CREATE SECRET s (SECRET 'ZZZZZZZZ')"}) {
    const std::string text = query;
    for (size_t n = 0; n <= text.size(); n++) {
      const std::string out = RedactSecretStatements(text.substr(0, n));
      EXPECT_EQ(out.find('Z'), std::string::npos) << out;
    }
  }
}

// The same through the real truncation, with the cut inside the secret.
TEST(MotherDuckRedactTest, SecretAtTheTruncationPointIsNotLogged) {
  InstallMotherDuckQueryRedactor();
  // One shape is enough (each cut is a full pass over 512 KiB); the prefix test above
  // covers the rest.
  for (const char* tail : {"' AS x; ATTACH 'postgresql://user:ZZ9ZZ9ZZ9@host/db'"}) {
    const size_t secret = std::string(tail).find("ZZ9");
    const std::string query =
        "SELECT '" + std::string(kMaxQueryBytes - 8 - secret - 3, 'a') + tail;
    const std::string out = RedactQuery(query);
    EXPECT_NE(out.find("[truncated]"), std::string::npos);
    EXPECT_EQ(out.find("ZZ9"), std::string::npos) << tail;
  }
  SetQueryRedactor(nullptr);
}

// libpq's default error message echoes the statement ("LINE 1: ..."); ours must not.
TEST(MotherDuckRedactTest, ErrorTextOmitsStatementEcho) {
  const char* uri = std::getenv("ADBC_POSTGRESQL_TEST_URI");
  if (!uri) GTEST_SKIP() << "ADBC_POSTGRESQL_TEST_URI not set";
  // The driver adds the token; so does this.
  const char* token = std::getenv("MOTHERDUCK_TOKEN");
  const char* keywords[] = {"dbname", "password", nullptr};
  const char* values[] = {uri, token != nullptr ? token : "", nullptr};
  PGconn* conn = PQconnectdbParams(keywords, values, /*expand_dbname=*/1);
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    GTEST_SKIP() << "cannot connect with libpq directly";
  }
  PGresult* result = PQexec(conn, "SELECT FROM WHERE 'UNIQUEMARKER' +");
  ASSERT_EQ(PQresultStatus(result), PGRES_FATAL_ERROR);
  if (std::string(PQresultErrorMessage(result)).find("UNIQUEMARKER") ==
      std::string::npos) {
    PQclear(result);
    PQfinish(conn);
    GTEST_SKIP() << "server did not echo the statement; nothing to check";
  }
  const std::string text = PqErrorText(result, conn);
  EXPECT_EQ(text.find("UNIQUEMARKER"), std::string::npos) << text;
  EXPECT_EQ(text.find("LINE"), std::string::npos) << text;
  PQclear(result);
  PQfinish(conn);
}

}  // namespace adbcpq
