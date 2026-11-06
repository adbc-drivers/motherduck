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

// Pipelined execution for MotherDuck: multi-statement queries run as separate
// commands, and bound rows are sent without waiting on each in turn.

#include <gtest/gtest.h>

#include "error.h"
#include "motherduck_redact.h"

namespace adbcpq {

TEST(MotherDuckRedactTest, SecretStatements) {
  EXPECT_EQ(RedactSecretStatements("SELECT 1"), "SELECT 1");
  EXPECT_EQ(RedactSecretStatements("CREATE TABLE secrets (a INT)"), "CREATE TABLE secrets (a INT)");
  EXPECT_EQ(RedactSecretStatements("CREATE SECRET s (TYPE s3, KEY_ID 'k', SECRET 'x')"),
            "CREATE SECRET s (...)");
  EXPECT_EQ(RedactSecretStatements("  create or replace persistent secret s IN motherduck "
                        "(TYPE s3, SECRET 'x')"),
            "  create or replace persistent secret s IN motherduck (...)");
  EXPECT_EQ(RedactSecretStatements("SELECT 1; CREATE TEMPORARY SECRET (TYPE s3, SECRET 'x');"),
            "SELECT 1; CREATE TEMPORARY SECRET (...)");
  EXPECT_EQ(RedactSecretStatements("ALTER SECRET s (SECRET 'x')"), "ALTER SECRET s (...)");
}

TEST(MotherDuckRedactTest, InstalledRedactorAppliesToErrors) {
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET s (SECRET 'x')");
  InstallMotherDuckQueryRedactor();
  EXPECT_EQ(RedactQuery("CREATE SECRET s (SECRET 'x')"), "CREATE SECRET s (...)");
  SetQueryRedactor(nullptr);
}

}  // namespace adbcpq
