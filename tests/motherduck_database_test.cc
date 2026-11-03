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
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include "motherduck_connection.h"
#include "motherduck_database.h"
#include "motherduck_factory.h"
#include "motherduck_statement.h"
#include "motherduck_test_util.h"
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

TEST(MotherDuckAttachModeTest, AddSessionOption) {
  EXPECT_EQ("--attach_mode=single",
            adbcpq::AddSessionOption("", "attach_mode", "single"));
  EXPECT_EQ("--session_name=x --attach_mode=workspace",
            adbcpq::AddSessionOption("--session_name=x", "attach_mode", "workspace"));
  // Options from the URI win.
  for (const char* options :
       {"--attach_mode=workspace", "-c attach_mode=workspace", "-cattach_mode=workspace",
        "--attach-mode=workspace", "-c attach-mode=workspace"}) {
    EXPECT_EQ(options, adbcpq::AddSessionOption(options, "attach_mode", "single"));
  }
  // Other options that mention attach_mode don't count.
  EXPECT_EQ(
      "--session_name=attach_mode=x --attach_mode=single",
      adbcpq::AddSessionOption("--session_name=attach_mode=x", "attach_mode", "single"));
  EXPECT_EQ(
      "--session_name=attach_mode --attach_mode=single",
      adbcpq::AddSessionOption("--session_name=attach_mode", "attach_mode", "single"));
  EXPECT_EQ("--session_name=a\\ attach_mode=x --attach_mode=single",
            adbcpq::AddSessionOption("--session_name=a\\ attach_mode=x", "attach_mode",
                                     "single"));
  // A bare <name>=<value> isn't a setting unless it follows -c.
  EXPECT_EQ("attach_mode=x --attach_mode=single",
            adbcpq::AddSessionOption("attach_mode=x", "attach_mode", "single"));
  EXPECT_EQ("-c foo=1 attach_mode=x --attach_mode=single",
            adbcpq::AddSessionOption("-c foo=1 attach_mode=x", "attach_mode", "single"));
}

// The status of getting a database option, for options that may be unset.
AdbcStatusCode GetOptionStatus(struct AdbcDatabase* database, const char* key,
                               struct AdbcError* error) {
  char buffer[128];
  size_t length = sizeof(buffer);
  return AdbcDatabaseGetOption(database, key, buffer, &length, error);
}

TEST(MotherDuckSessionNameTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
                                    "user123", &error),
              IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME, &error),
              ::testing::Optional("user123"s));
  // An empty value unsets it.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
                                    "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckSessionNameTest, OptionInvalid) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* value : {"a b", "a\tb", "a\\b"}) {
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value,
                                      ADBC_MOTHERDUCK_OPTION_SESSION_NAME, value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_SESSION_NAME, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  EXPECT_THAT(error.message, ::testing::HasSubstr("is not set"));
  if (error.release) error.release(&error);
}

TEST(MotherDuckRegionTest, RegionHost) {
  EXPECT_EQ("pg.us-east-1-aws.motherduck.com", adbcpq::RegionHost("us-east-1"));
  EXPECT_EQ("pg.eu-central-1-aws.motherduck.com", adbcpq::RegionHost("eu-central-1"));
  // A region can name its cloud.
  EXPECT_EQ("pg.us-east-1-aws.motherduck.com", adbcpq::RegionHost("us-east-1-aws"));
  EXPECT_EQ("pg.us-central1-gcp.motherduck.com", adbcpq::RegionHost("us-central1-gcp"));
}

TEST(MotherDuckRegionTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_REGION, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_REGION,
                                    "us-west-2", &error),
              IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(&database.value,
                                                 ADBC_MOTHERDUCK_OPTION_REGION, &error),
              ::testing::Optional("us-west-2"s));
  // An empty value unsets it.
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_REGION, "", &error),
      IsOkStatus(&error));
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_REGION, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckRegionTest, OptionInvalid) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* value :
       {"US-EAST-1", "us east 1", "us-east-1.example.com", "us-east-1/x", "us_east_1",
        "-", "--", "-us-east-1", "us-east-1-", "us--east-1", "aws", "useast", "useast1",
        "us-east",
        "us-\xe9"
        "ast-1"}) {
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_REGION,
                                      value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_REGION, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckRegionTest, ConflictsWithHost) {
  // Fails before connecting, so this needs no server.
  for (const char* uri :
       {"postgresql://postgres@localhost:1/db", "host=localhost dbname=db",
        "hostaddr=127.0.0.1 dbname=db", "dbname=db"}) {
    // Without a host in the URI, PGHOSTADDR conflicts.
    ScopedEnv pghostaddr("PGHOSTADDR",
                         std::strcmp(uri, "dbname=db") == 0 ? "127.0.0.1" : nullptr);
    struct AdbcError error = {};
    adbc_validation::Handle<struct AdbcDatabase> database;
    ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri", uri, &error),
                IsOkStatus(&error));
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_REGION,
                                      "us-east-1", &error),
                IsOkStatus(&error));
    EXPECT_THAT(AdbcDatabaseInit(&database.value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    EXPECT_THAT(error.message, ::testing::HasSubstr("not both"));
    if (error.release) error.release(&error);
  }
}

TEST(MotherDuckUsernameTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_OPTION_USERNAME, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_OPTION_USERNAME, "postgres", &error),
      IsOkStatus(&error));
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("postgres"s));
}

TEST(MotherDuckPasswordTest, RequiresToken) {
  // Fails before connecting, so this needs no server.
  ScopedEnv pgpassword("PGPASSWORD", "token");
  ScopedEnv motherduck_token("MOTHERDUCK_TOKEN", nullptr);
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri",
                                    "postgresql://postgres@localhost:1/db", &error),
              IsOkStatus(&error));
  EXPECT_THAT(AdbcDatabaseInit(&database.value, &error),
              IsStatus(ADBC_STATUS_UNAUTHENTICATED, &error));
  EXPECT_THAT(error.message, ::testing::HasSubstr("No MotherDuck token"));
  if (error.release) error.release(&error);
}

TEST(MotherDuckPasswordTest, OptionNotReadable) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_OPTION_PASSWORD, "secret", &error),
      IsOkStatus(&error));
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_OPTION_PASSWORD, &error),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  EXPECT_THAT(error.message, ::testing::HasSubstr("can't be read"));
  if (error.release) error.release(&error);
}

// The libpq parameters Connect would pass, for a database with the given URI
// and options. Needs no server.
class MotherDuckConnectParamsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_THAT(database_.SetOption(ADBC_OPTION_PASSWORD, "token", &error_),
                IsOkStatus(&error_));
  }
  void TearDown() override {
    if (error_.release) error_.release(&error_);
  }

  void SetOption(const char* key, const char* value) {
    ASSERT_THAT(database_.SetOption(key, value, &error_), IsOkStatus(&error_));
  }

  // Every value passed for keyword, in order.
  std::vector<std::string> Values(const char* keyword) {
    std::vector<std::pair<std::string, std::string>> params;
    EXPECT_THAT(database_.ConnectParams(&params, &error_), IsOkStatus(&error_));
    std::vector<std::string> values;
    for (const auto& [key, value] : params) {
      if (key == keyword) values.push_back(value);
    }
    return values;
  }

  ScopedEnv pgoptions_{"PGOPTIONS", nullptr};
  ScopedEnv pgsslmode_{"PGSSLMODE", nullptr};
  ScopedEnv pguser_{"PGUSER", "someone"};
  adbcpq::MotherduckDatabase database_;
  struct AdbcError error_ = {};
};

TEST_F(MotherDuckConnectParamsTest, TokenAlias) {
  SetOption("uri", "postgresql://localhost/db?password=uri");
  SetOption(ADBC_MOTHERDUCK_OPTION_TOKEN, "alias");
  EXPECT_THAT(Values("password"), ::testing::ElementsAre("alias"));
  // It's the same option, so the last one set wins.
  SetOption(ADBC_OPTION_PASSWORD, "password");
  EXPECT_THAT(Values("password"), ::testing::ElementsAre("password"));
  EXPECT_THAT(
      database_.GetOption(ADBC_MOTHERDUCK_OPTION_TOKEN, nullptr, nullptr, &error_),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error_));
}

TEST_F(MotherDuckConnectParamsTest, UserDefaultsToPostgres) {
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("postgres"));
}

TEST_F(MotherDuckConnectParamsTest, UriUser) {
  SetOption("uri", "postgresql://alice@localhost/db");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
}

TEST_F(MotherDuckConnectParamsTest, UsernameReplacesUriUser) {
  SetOption("uri", "postgresql://alice@localhost/db");
  SetOption(ADBC_OPTION_USERNAME, "bob");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("bob"));
}

TEST_F(MotherDuckConnectParamsTest, SessionOptions) {
  SetOption("uri", "postgresql://localhost/db?options=-c%20x%3Dy");
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "adbc_session");
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre("-c x=y --attach_mode=single --session_name=adbc_session"));
}

TEST_F(MotherDuckConnectParamsTest, RegionRequiresTls) {
  SetOption("uri", "postgresql:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
}

TEST_F(MotherDuckConnectParamsTest, RegionKeepsSslmode) {
  SetOption("uri", "postgresql:///db?sslmode=verify-full");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  {
    ScopedEnv pgsslmode("PGSSLMODE", "verify-full");
    SetOption("uri", "postgresql:///db");
    EXPECT_THAT(Values("sslmode"), ::testing::IsEmpty());
  }
}

TEST_F(MotherDuckConnectParamsTest, NoRegionLeavesSslmode) {
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::IsEmpty());
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
