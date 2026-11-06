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

#include <gtest/gtest.h>
#include <libpq-fe.h>

#include "error.h"
#include "motherduck_redact.h"

namespace adbcpq {

TEST(MotherDuckRedactTest, SecretStatements) {
  EXPECT_EQ(RedactSecretStatements("SELECT 1"), "SELECT 1");
  EXPECT_EQ(RedactSecretStatements("CREATE TABLE secrets (a INT)"),
            "CREATE TABLE secrets (a INT)");
  EXPECT_EQ(RedactSecretStatements("CREATE SECRET s (TYPE s3, KEY_ID 'k', SECRET 'x')"),
            "CREATE SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("  create or replace persistent secret s IN "
                                   "motherduck (TYPE s3, SECRET 'x')"),
            "  create or replace persistent secret s IN motherduck (...)");
  EXPECT_EQ(RedactSecretStatements("CREATE OR REPLACE TEMPORARY SECRET IF NOT EXISTS s "
                                   "IN motherduck (SECRET 'x')"),
            "CREATE OR REPLACE TEMPORARY SECRET IF NOT EXISTS s IN motherduck (...)");
  EXPECT_EQ(RedactSecretStatements("SELECT 1; CREATE TEMPORARY SECRET (TYPE s3, "
                                   "SECRET 'x');"),
            "SELECT 1; CREATE TEMPORARY SECRET (...)");
  EXPECT_EQ(RedactSecretStatements("ALTER SECRET s (SECRET 'x')"),
            "ALTER SECRET s (...)");
}

TEST(MotherDuckRedactTest, CommentsAndShortForms) {
  EXPECT_EQ(RedactSecretStatements("CREATE /*c*/ SECRET s (SECRET 'x')"),
            "CREATE /*c*/ SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("CREATE --c\n SECRET s (SECRET 'x')"),
            "CREATE --c\n SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("CREATE TEMP SECRET s (SECRET 'x')"),
            "CREATE TEMP SECRET s (...)");
}

TEST(MotherDuckRedactTest, KeywordsInsideLiteralsAreIgnored) {
  const std::string query = "SELECT 'CREATE SECRET later' WHERE a IN (1)";
  EXPECT_EQ(RedactSecretStatements(query), query);
  // A commented-out statement is masked, but doesn't hide the live statement after it.
  EXPECT_EQ(RedactSecretStatements("SELECT 1 -- CREATE SECRET s (x)\nWHERE a IN (1)"),
            "SELECT 1 -- CREATE SECRET s (...)\nWHERE a IN (1)");
}

TEST(MotherDuckRedactTest, CredentialSettings) {
  EXPECT_EQ(RedactSecretStatements("SET s3_secret_access_key='abc'"),
            "SET s3_secret_access_key= ...");
  EXPECT_EQ(RedactSecretStatements("SET GLOBAL motherduck_token TO 'abc'"),
            "SET GLOBAL motherduck_token TO ...");
  EXPECT_EQ(RedactSecretStatements("SET threads = 4"), "SET threads = 4");
}

TEST(MotherDuckRedactTest, QuotedAndLookalikeSettings) {
  EXPECT_EQ(RedactSecretStatements("SET \"s3_secret_access_key\" = 'abc'"),
            "SET \"s3_secret_access_key\" = ...");
  EXPECT_EQ(RedactSecretStatements("SET monkey = 'x'"), "SET monkey = 'x'");
  EXPECT_EQ(RedactSecretStatements("SET tokenizer = 'icu'"), "SET tokenizer = 'icu'");
}

TEST(MotherDuckRedactTest, UrlUserinfoAndEscapes) {
  EXPECT_EQ(RedactSecretStatements("ATTACH 'postgresql://user:abc@host/db'"),
            "ATTACH 'postgresql://user:...@host/db'");
  EXPECT_EQ(RedactSecretStatements("SELECT 's3://AKIA:abc@bucket/path'"),
            "SELECT 's3://AKIA:...@bucket/path'");
  EXPECT_EQ(RedactSecretStatements("SELECT 'keyboard=abc'"), "SELECT 'keyboard=abc'");
  EXPECT_EQ(RedactSecretStatements("ATTACH E'md:db?motherduck_token=abc\\'def'"),
            "ATTACH E'md:db?motherduck_token=...'");
}

TEST(MotherDuckRedactTest, UnterminatedEscapeDoesNotThrow) {
  EXPECT_EQ(RedactSecretStatements("ATTACH E'md:db?motherduck_token=abc\\"),
            "ATTACH E'md:db?motherduck_token=...");
  EXPECT_EQ(RedactSecretStatements("SELECT E'abc\\"), "SELECT E'abc\\");
}

TEST(MotherDuckRedactTest, CommentsAreScrubbed) {
  EXPECT_EQ(RedactSecretStatements("-- motherduck_token=SUPERSECRET\n"
                                   "CREATE SECRET s (TYPE s3, SECRET 'x')"),
            "-- motherduck_token=...\nCREATE SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("/* password=SUPERSECRET */ SELECT 1"),
            "/* password=...*/ SELECT 1");
  EXPECT_EQ(RedactSecretStatements("SELECT 'md:db?motherduck_token=LEAK' x "
                                   "CREATE SECRET s (SECRET 'x')"),
            "SELECT 'md:db?motherduck_token=...' x CREATE SECRET s (...)");
}

TEST(MotherDuckRedactTest, CredentialNamesInParameters) {
  EXPECT_EQ(RedactSecretStatements("SELECT 's3://b/o?access_key_id=AKIA&"
                                   "secret_access_key=sekret'"),
            "SELECT 's3://b/o?access_key_id=...&secret_access_key=...'");
  EXPECT_EQ(RedactSecretStatements("SELECT 'https://x?key_id=AKIA&token=ok'"),
            "SELECT 'https://x?key_id=...&token=...'");
  EXPECT_EQ(RedactSecretStatements("ATTACH 'md:db?motherduck_token=my secret'"),
            "ATTACH 'md:db?motherduck_token=...'");
}

TEST(MotherDuckRedactTest, DollarQuotedBodies) {
  EXPECT_EQ(RedactSecretStatements("CREATE MACRO m() AS $$ CREATE SECRET s (TYPE s3, "
                                   "SECRET 'supersecret') $$"),
            "CREATE MACRO m() AS $$ CREATE SECRET s (...)$$");
  // $foo bar$ is not a dollar quote, so the secret statement is seen.
  EXPECT_EQ(RedactSecretStatements("SELECT $foo bar$ CREATE SECRET s (SECRET 'x')"),
            "SELECT $foo bar$ CREATE SECRET s (...)");
}

TEST(MotherDuckRedactTest, CommentedOutStatements) {
  EXPECT_EQ(RedactSecretStatements("-- CREATE SECRET s (TYPE s3, KEY_ID 'AKIA', "
                                   "SECRET 'wJalr')"),
            "-- CREATE SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("-- motherduck_token='SUPERSECRET'\nSELECT 1"),
            "-- motherduck_token=...\nSELECT 1");
  EXPECT_EQ(RedactSecretStatements("-- motherduck_token = SUPERSECRET"),
            "-- motherduck_token = ...");
  EXPECT_EQ(RedactSecretStatements("/* token=\"SUPERSECRET\" */ SELECT 1"),
            "/* token=... */ SELECT 1");
  EXPECT_EQ(RedactSecretStatements("SELECT \"md:db?token='SUPERSECRET'\""),
            "SELECT \"md:db?token=...\"");
  EXPECT_EQ(RedactSecretStatements("SELECT $$ -- CREATE SECRET s (SECRET 'wJalr') $$"),
            "SELECT $$ -- CREATE SECRET s (...)$$");
}

TEST(MotherDuckRedactTest, QuotedAndDottedSettingNames) {
  EXPECT_EQ(RedactSecretStatements("SET 's3_secret_access_key' = 'SUPERSECRET'"),
            "SET 's3_secret_access_key' = ...");
  EXPECT_EQ(RedactSecretStatements("SET memory.s3_secret_access_key = 'SUPERSECRET'"),
            "SET memory.s3_secret_access_key = ...");
  EXPECT_EQ(RedactSecretStatements("SET memory.threads = 4"), "SET memory.threads = 4");
}

TEST(MotherDuckRedactTest, ConnectionStringTokens) {
  EXPECT_EQ(RedactSecretStatements("ATTACH 'md:db?motherduck_token=abc.def&x=1'"),
            "ATTACH 'md:db?motherduck_token=...&x=1'");
  EXPECT_EQ(RedactSecretStatements("SELECT 'a=b'"), "SELECT 'a=b'");
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
