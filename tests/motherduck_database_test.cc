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

// Tests for the MotherDuck database subclass that don't need a server.

#include <cstdint>
#include <memory>
#include <string>

#include <arrow-adbc/adbc.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include "motherduck_connection.h"
#include "motherduck_database.h"
#include "motherduck_factory.h"
#include "motherduck_statement.h"
#include "motherduck_type_resolver.h"
#include "postgres_type.h"
#include "validation/adbc_validation.h"
#include "validation/adbc_validation_util.h"

using adbc_validation::IsOkStatus;
using adbc_validation::IsStatus;
using std::string_literals::operator""s;

TEST(MotherDuckAttachModeTest, OptionDefault) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, &error),
              ::testing::Optional("single"s));
}

TEST(MotherDuckAttachModeTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const std::string value : {"workspace", "single"}) {
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      value.c_str(), &error),
                IsOkStatus(&error));
    EXPECT_THAT(adbc_validation::DatabaseGetOption(
                    &database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, &error),
                ::testing::Optional(value));
  }
}

TEST(MotherDuckAttachModeTest, OptionInvalid) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* value : {"", "SINGLE", "multi"}) {
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, &error),
              ::testing::Optional("single"s));
}

TEST(MotherDuckAttachModeTest, AddAttachMode) {
  EXPECT_EQ("--attach_mode=single", adbcpq::AddAttachMode("", "single"));
  EXPECT_EQ("--session_name=x --attach_mode=workspace",
            adbcpq::AddAttachMode("--session_name=x", "workspace"));
  // Options from the URI win.
  EXPECT_EQ("--attach_mode=workspace",
            adbcpq::AddAttachMode("--attach_mode=workspace", "single"));
  EXPECT_EQ("-c attach_mode=workspace",
            adbcpq::AddAttachMode("-c attach_mode=workspace", "single"));
  EXPECT_EQ("-cattach_mode=workspace",
            adbcpq::AddAttachMode("-cattach_mode=workspace", "single"));
  EXPECT_EQ("--attach-mode=workspace",
            adbcpq::AddAttachMode("--attach-mode=workspace", "single"));
  EXPECT_EQ("-c attach-mode=workspace",
            adbcpq::AddAttachMode("-c attach-mode=workspace", "single"));
  // Other options that mention attach_mode don't count.
  EXPECT_EQ("--session_name=attach_mode=x --attach_mode=single",
            adbcpq::AddAttachMode("--session_name=attach_mode=x", "single"));
  EXPECT_EQ("--session_name=attach_mode --attach_mode=single",
            adbcpq::AddAttachMode("--session_name=attach_mode", "single"));
}

// The PostgreSQL driver's type resolver mode (and the server type table it
// selects) must not show through the MotherDuck driver.
TEST(MotherDuckPostgresOptionsTest, TypeResolverModeNotReported) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  const char* key = "adbc.postgresql.type_resolver_mode";

  char value[64];
  size_t length = sizeof(value);
  EXPECT_THAT(AdbcDatabaseGetOption(&database.value, key, value, &length, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);

  ::testing::internal::CaptureStderr();
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, key, "server", &error),
              IsOkStatus(&error));
  ::testing::internal::GetCapturedStderr();
  length = sizeof(value);
  EXPECT_THAT(AdbcDatabaseGetOption(&database.value, key, value, &length, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

// The ADBC entry points must create the MotherDuck subclasses, never the
// PostgreSQL classes they derive from.
TEST(MotherDuckFactoryTest, CreatesMotherduckClasses) {
  EXPECT_NE(dynamic_cast<adbcpq::MotherduckDatabase*>(adbcpq::MakeDatabase().get()),
            nullptr);
  EXPECT_NE(dynamic_cast<adbcpq::MotherduckConnection*>(adbcpq::MakeConnection().get()),
            nullptr);
  EXPECT_NE(dynamic_cast<adbcpq::MotherduckStatement*>(adbcpq::MakeStatement().get()),
            nullptr);
}

TEST(MotherDuckTypeResolverTest, DuckDBTypes) {
  adbcpq::PostgresTypeResolver resolver;
  ASSERT_TRUE(adbcpq::InitializeMotherduckTypeResolver(resolver).ok());

  // The standard OIDs still win the reverse lookups
  EXPECT_EQ(resolver.GetOID(adbcpq::PostgresTypeId::kText), 25);

  adbcpq::PostgresType type;
  ASSERT_EQ(resolver.Find(17001, &type, nullptr), NANOARROW_OK);
  EXPECT_EQ(type.type_id(), adbcpq::PostgresTypeId::kText);
  EXPECT_EQ(type.typname(), "duckdb_struct");

  ASSERT_EQ(resolver.Find(17002, &type, nullptr), NANOARROW_OK);
  EXPECT_EQ(type.type_id(), adbcpq::PostgresTypeId::kArray);
  EXPECT_EQ(type.typname(), "_duckdb_struct");
  ASSERT_EQ(type.n_children(), 1);
  EXPECT_EQ(type.child(0).oid(), 17001);

  for (uint32_t oid = 17000; oid <= 17018; oid++) {
    SCOPED_TRACE("oid = " + std::to_string(oid));
    EXPECT_EQ(resolver.Find(oid, &type, nullptr), NANOARROW_OK);
  }
}

TEST(MotherDuckTypeResolverTest, Shared) {
  std::shared_ptr<adbcpq::PostgresTypeResolver> first, second;
  ASSERT_TRUE(adbcpq::SharedMotherduckTypeResolver(&first).ok());
  ASSERT_TRUE(adbcpq::SharedMotherduckTypeResolver(&second).ok());
  EXPECT_EQ(first.get(), second.get());
}
