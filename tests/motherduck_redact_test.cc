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

#include <gtest/gtest.h>

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
  const std::string commented = "SELECT 1 -- CREATE SECRET s (x)\nWHERE a IN (1)";
  EXPECT_EQ(RedactSecretStatements(commented), commented);
}

TEST(MotherDuckRedactTest, CredentialSettings) {
  EXPECT_EQ(RedactSecretStatements("SET s3_secret_access_key='abc'"),
            "SET s3_secret_access_key= ...");
  EXPECT_EQ(RedactSecretStatements("SET GLOBAL motherduck_token TO 'abc'"),
            "SET GLOBAL motherduck_token TO ...");
  EXPECT_EQ(RedactSecretStatements("SET threads = 4"), "SET threads = 4");
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

}  // namespace adbcpq
