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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include "adbc_driver_motherduck_config.h"
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
  // An empty value unsets it, which reads back as the default.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                    "workspace", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                    "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, &error),
              ::testing::Optional("single"s));
}

TEST(MotherDuckAttachModeTest, OptionInvalid) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* value : {"SINGLE", "multi"}) {
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
                                      value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, &error),
              ::testing::Optional("single"s));
}

TEST(MotherDuckAttachModeTest, SessionOption) {
  EXPECT_EQ("--attach_mode=single", adbcpq::SessionOption("attach_mode", "single"));
  // Values are escaped for libpq.
  EXPECT_EQ("--session_name=a\\ b\\\tc\\\\d",
            adbcpq::SessionOption("session_name", "a b\tc\\d"));
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

TEST(MotherDuckApplicationNameTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME,
                            "dbt", &error),
      IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, &error),
              ::testing::Optional("dbt"s));
  // An empty value unsets it.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value,
                                    ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckConnectTimeoutTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  for (const char* timeout : {"10", "0", "999999999"}) {
    ASSERT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                              timeout, &error),
        IsOkStatus(&error));
    EXPECT_THAT(adbc_validation::DatabaseGetOption(
                    &database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
                ::testing::Optional(std::string(timeout)));
  }
  // An empty value unsets it.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value,
                                    ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckConnectTimeoutTest, LeadingZeros) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const auto& [written, read] :
       {std::pair{"007", "7"}, std::pair{"000", "0"}, std::pair{"0010", "10"},
        // Leading zeros don't count towards the 9 digits.
        std::pair{"0000000010", "10"}, std::pair{"000000000000000000000", "0"},
        std::pair{"0999999999", "999999999"}}) {
    ASSERT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                              written, &error),
        IsOkStatus(&error));
    // The string and integer getters agree.
    EXPECT_THAT(adbc_validation::DatabaseGetOption(
                    &database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
                ::testing::Optional(std::string(read)));
  }
}

TEST(MotherDuckConnectTimeoutTest, OptionInvalid) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* timeout :
       {"-1", "1.5", "ten", "10s", " 10", "1000000000", "01000000000"}) {
    SCOPED_TRACE(timeout);
    EXPECT_THAT(
        AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                              timeout, &error),
        IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
    error = {};
  }
  // A rejected value keeps the previous one.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value,
                                    ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "5", &error),
              IsOkStatus(&error));
  EXPECT_THAT(AdbcDatabaseSetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "ten", &error),
              IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
  if (error.release) error.release(&error);
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
              ::testing::Optional(std::string("5")));
}

// Python's driver manager sends an int through the integer setter.
TEST(MotherDuckConnectTimeoutTest, IntegerOption) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  int64_t timeout = -1;
  EXPECT_THAT(
      AdbcDatabaseGetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                               &timeout, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  for (int64_t value : {10, 0, 999999999}) {
    ASSERT_THAT(
        AdbcDatabaseSetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                                 value, &error),
        IsOkStatus(&error));
    ASSERT_THAT(
        AdbcDatabaseGetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                                 &timeout, &error),
        IsOkStatus(&error));
    EXPECT_EQ(timeout, value);
    // The string getter agrees.
    EXPECT_THAT(adbc_validation::DatabaseGetOption(
                    &database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, &error),
                ::testing::Optional(std::to_string(value)));
  }
  // Values the string setter rejects fail the same way, and keep the last one.
  for (int64_t value : {int64_t{-1}, int64_t{1000000000}}) {
    EXPECT_THAT(
        AdbcDatabaseSetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                                 value, &error),
        IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  ASSERT_THAT(
      AdbcDatabaseGetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                               &timeout, &error),
      IsOkStatus(&error));
  EXPECT_EQ(timeout, 999999999);
  // A null output pointer is an error, not a crash.
  EXPECT_THAT(
      AdbcDatabaseGetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
                               nullptr, &error),
      IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
  if (error.release) error.release(&error);
  // Other options aren't integers.
  EXPECT_THAT(
      AdbcDatabaseSetOptionInt(&database.value, ADBC_MOTHERDUCK_OPTION_REGION, 1, &error),
      IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error));
  if (error.release) error.release(&error);
}

TEST(MotherDuckRegionTest, RegionHost) {
  // MotherDuck's AWS regions.
  for (const char* region : {"us-east-1", "us-west-2", "eu-central-1", "eu-west-1",
                             "ap-northeast-1", "ap-southeast-2"}) {
    EXPECT_EQ("pg." + std::string(region) + "-aws.motherduck.com",
              adbcpq::RegionHost(region));
  }
  // Azure region names have no dashes.
  EXPECT_EQ("pg.eastus2-azure.motherduck.com", adbcpq::RegionHost("eastus2"));
  EXPECT_EQ("pg.westeurope-azure.motherduck.com", adbcpq::RegionHost("westeurope"));
  // A region can name its cloud.
  EXPECT_EQ("pg.us-east-1-aws.motherduck.com", adbcpq::RegionHost("us-east-1-aws"));
  EXPECT_EQ("pg.eastus2-azure.motherduck.com", adbcpq::RegionHost("eastus2-azure"));
  EXPECT_EQ("pg.westeurope-azure.motherduck.com", adbcpq::RegionHost("westeurope-azure"));
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
       {"us east 1", "us-east-1.example.com", "us-east-1/x", "us_east_1", "-", "--",
        "-us-east-1", "us-east-1-", "us--east-1",
        // Too long for a hostname label once -azure is added.
        "a23456789012345678901234567890123456789012345678901234567890",
        // libpq would also try each comma-separated host.
        "x,attacker.example,", "us-east-1,localhost",
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
  for (const char* uri : {"motherduck://postgres@localhost:1/db",
                          "motherduck://localhost/db", "motherduck:///db"}) {
    // Without a host in the URI, PGHOSTADDR conflicts.
    ScopedEnv pghostaddr(
        "PGHOSTADDR", std::strcmp(uri, "motherduck:///db") == 0 ? "127.0.0.1" : nullptr);
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
  // It reports the user the driver connects as: postgres by default, then the
  // URI's user, then the option.
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("postgres"s));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri",
                                    "motherduck://alice@localhost/db", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("alice"s));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_OPTION_USERNAME, "bob", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("bob"s));
}

// A URI libpq can't parse is rejected when it's set, and the previous one stays.
TEST(MotherDuckUsernameTest, UnparseableUri) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri",
                                    "motherduck://alice@localhost/db", &error),
              IsOkStatus(&error));
  for (const char* uri : {"motherduck://[bad", "md:my%zz", "md:my db",
                          "motherduck://alice@localhost/db%2"}) {
    SCOPED_TRACE(uri);
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, "uri", uri, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
    error = {};
  }
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("alice"s));
}

TEST(MotherDuckPasswordTest, RequiresToken) {
  // Fails before connecting, so this needs no server.
  ScopedEnv pgpassword("PGPASSWORD", "token");
  ScopedEnv motherduck_token("MOTHERDUCK_TOKEN", nullptr);
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri",
                                    "motherduck://postgres@localhost:1/db", &error),
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

// The libpq parameters Connect would pass. Needs no server.
class MotherDuckConnectParamsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_THAT(database_.SetOption(ADBC_OPTION_PASSWORD, "token", &error_),
                IsOkStatus(&error_));
    SetCaBundle(kBundle);
  }
  void TearDown() override {
    if (error_.release) error_.release(&error_);
  }

  void SetOption(const char* key, const char* value) {
    ASSERT_THAT(database_.SetOption(key, value, &error_), IsOkStatus(&error_));
  }

  // The status ConnectParams returns.
  AdbcStatusCode Status() {
    std::vector<std::pair<std::string, std::string>> params;
    AdbcStatusCode status = database_.ConnectParams(&params, &error_);
    if (error_.release) error_.release(&error_);
    return status;
  }

  // The CA bundle the driver finds when the connection names none; empty for
  // none.
  void SetCaBundle(std::string bundle) {
    database_.SetCaBundleFinder([bundle] { return bundle; });
  }

  // Every value passed for keyword, in order. Keeps what the driver printed in
  // stderr_.
  std::vector<std::string> Values(const char* keyword) {
    std::vector<std::pair<std::string, std::string>> params;
    ::testing::internal::CaptureStderr();
    EXPECT_THAT(database_.ConnectParams(&params, &error_), IsOkStatus(&error_));
    stderr_ = ::testing::internal::GetCapturedStderr();
    std::vector<std::string> values;
    for (const auto& [key, value] : params) {
      if (key == keyword) values.push_back(value);
    }
    return values;
  }

  ScopedEnv pgoptions_{"PGOPTIONS", nullptr};
  ScopedEnv pgsslmode_{"PGSSLMODE", nullptr};
  ScopedEnv pgsslrootcert_{"PGSSLROOTCERT", nullptr};
  ScopedEnv pgservice_{"PGSERVICE", nullptr};
  ScopedEnv pghostaddr_{"PGHOSTADDR", nullptr};
  ScopedEnv pgport_{"PGPORT", "5433"};
  ScopedEnv pguser_{"PGUSER", "someone"};
  // A bad SSL_CERT_FILE in the developer's environment would fail every test.
  ScopedEnv ssl_cert_file_{"SSL_CERT_FILE", nullptr};
  // Where libpq looks for its default root certificate, ~/.postgresql/root.crt
  ScopedEnv home_{"HOME", "/nonexistent"};
  ScopedEnv appdata_{"APPDATA", "C:\\nonexistent"};
  static constexpr char kBundle[] = "/found/ca-bundle.pem";
  adbcpq::MotherduckDatabase database_;
  struct AdbcError error_ = {};
  std::string stderr_;
};

TEST_F(MotherDuckConnectParamsTest, TokenAlias) {
  SetOption("uri", "motherduck://alice:uri@localhost/db");
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
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("postgres"));
}

TEST_F(MotherDuckConnectParamsTest, UriUser) {
  SetOption("uri", "motherduck://alice@localhost/db");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
}

TEST(MotherDuckUriTest, NormalizeUri) {
  EXPECT_EQ("postgresql://localhost/db",
            adbcpq::NormalizeUri("motherduck://localhost/db"));
  EXPECT_EQ("postgresql://a:b@h:1/db?x=y",
            adbcpq::NormalizeUri("motherduck://a:b@h:1/db?x=y"));
  EXPECT_EQ("postgresql:///", adbcpq::NormalizeUri("motherduck:///"));
  // md:// is the short form.
  EXPECT_EQ("postgresql://localhost/db", adbcpq::NormalizeUri("md://localhost/db"));
  EXPECT_EQ("postgresql://a:b@h:1/db?x=y", adbcpq::NormalizeUri("md://a:b@h:1/db?x=y"));
  EXPECT_EQ("postgresql:///db", adbcpq::NormalizeUri("md:///db"));
  // md:[database][?parameters] is DuckDB's form; md: alone is the workspace.
  EXPECT_EQ("postgresql:///my_db", adbcpq::NormalizeUri("md:my_db"));
  EXPECT_EQ("postgresql:///my_db?x=y", adbcpq::NormalizeUri("md:my_db?x=y"));
  EXPECT_EQ("postgresql:///md%3A", adbcpq::NormalizeUri("md:"));
  EXPECT_EQ("postgresql:///md%3A?x=y", adbcpq::NormalizeUri("md:?x=y"));
  // Anything else is returned as is.
  for (const char* uri :
       {"postgresql://localhost/db", "postgres://localhost/db",
        "host=localhost dbname=db", "motherduck:db", "motherduck:/", "", "motherduck:/x",
        "md:/x", "md:token@db", "md:pg.us-east-1-aws.motherduck.com/my_db", "md:db/x?a=b",
        // The scheme is case-sensitive, like libpq's.
        "MotherDuck://localhost/db", "MD://localhost/db"}) {
    EXPECT_EQ(uri, adbcpq::NormalizeUri(uri));
  }
}

TEST(MotherDuckUriTest, RejectsOtherSchemes) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* uri :
       {"postgresql://localhost/db", "postgres://localhost/db",
        "host=localhost dbname=db", "motherduck:db", "md:/db", "md:eyJ_SECRET@my_db",
        "md:pg.us-east-1-aws.motherduck.com/my_db", "md:my_db/schema", "MD:db",
        "MotherDuck://localhost/db", "motherduck:/db"}) {
    SCOPED_TRACE(uri);
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, "uri", uri, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    EXPECT_THAT(error.message,
                ::testing::HasSubstr("expected a motherduck://, md:// or md: URI"));
    if (error.release) error.release(&error);
    error = {};
  }
  // An empty value unsets the URI, as it did before.
  EXPECT_THAT(AdbcDatabaseSetOption(&database.value, "uri", "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(AdbcDatabaseSetOption(&database.value, "uri", "motherduck:///db", &error),
              IsOkStatus(&error));
}

TEST_F(MotherDuckConnectParamsTest, RejectedUriKeepsPreviousUri) {
  SetOption("uri", "motherduck://alice@localhost/db");
  EXPECT_EQ(database_.SetOption("uri", "postgresql://bob:secret@elsewhere/db", &error_),
            ADBC_STATUS_INVALID_ARGUMENT);
  // The message doesn't repeat the rejected URI, which may hold a token.
  EXPECT_THAT(error_.message, ::testing::Not(::testing::HasSubstr("secret")));
  EXPECT_THAT(error_.message, ::testing::Not(::testing::HasSubstr("elsewhere")));
  if (error_.release) error_.release(&error_);
  error_ = {};
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("localhost"));
}

TEST_F(MotherDuckConnectParamsTest, UriParts) {
  SetOption("uri", "motherduck://alice:secret@localhost:5000/db");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("localhost"));
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("5000"));
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("db"));
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
  // The fixture's password option wins over the URI's.
  EXPECT_THAT(Values("password"), ::testing::ElementsAre("token"));
}

TEST_F(MotherDuckConnectParamsTest, ShortScheme) {
  SetOption("uri", "md://alice@localhost:5000/db");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("localhost"));
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("5000"));
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("db"));
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
  SetOption("uri", "md:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
}

TEST_F(MotherDuckConnectParamsTest, ShortFormWithoutSlashes) {
  // md:database has no host, so the region supplies it.
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  SetOption("uri", "md:my_db?motherduck.connect_timeout=3");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("my_db"));
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("3"));
  // md: alone is the workspace database.
  SetOption("uri", "md:");
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("md:"));
}

// Query parameters named like the options set them; everything else is an
// error, and libpq never sees the query.
TEST_F(MotherDuckConnectParamsTest, UriQuerySetsOptions) {
  const std::string driver = "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  SetOption("uri",
            "motherduck:///db?motherduck.region=eu-central-1"
            "&motherduck.attach_mode=workspace&motherduck.session_name=my%20session"
            "&motherduck.application_name=dbt&motherduck.connect_timeout=7"
            "&motherduck.tls_verify=false");
  EXPECT_THAT(Values("host"),
              ::testing::ElementsAre("pg.eu-central-1-aws.motherduck.com"));
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=workspace "
                             "--session_name=my\\ session"));
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("dbt " + driver));
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("7"));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  // The query isn't passed to libpq.
  EXPECT_THAT(Values("motherduck.region"), ::testing::IsEmpty());
  // The options read back as set.
  char value[64];
  size_t length = sizeof(value);
  ASSERT_THAT(database_.GetOption(ADBC_MOTHERDUCK_OPTION_REGION, value, &length, &error_),
              IsOkStatus(&error_));
  EXPECT_STREQ(value, "eu-central-1");
  // The URI itself can't be read back: it may hold a token.
  length = sizeof(value);
  EXPECT_THAT(database_.GetOption("uri", value, &length, &error_),
              IsStatus(ADBC_STATUS_NOT_FOUND, &error_));
  if (error_.release) error_.release(&error_);
  error_ = {};
}

TEST_F(MotherDuckConnectParamsTest, OptionsOutrankUriQuery) {
  const std::string driver = "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  // Whichever is set first, the option wins.
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "option");
  SetOption("uri",
            "motherduck://localhost/db?motherduck.application_name=uri"
            "&motherduck.connect_timeout=3&motherduck.tls_verify=false");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("option " + driver));
  // Unset, the URI's value applies again.
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("uri " + driver));
  SetOption(ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "10");
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("10"));
  SetOption(ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "");
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("3"));
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "true");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  // Query parameters are percent-decoded, and an empty value is unset.
  SetOption("uri", "motherduck://localhost/db?motherduck.application_name=a%26b%3Dc");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("a&b=c " + driver));
  SetOption("uri", "motherduck://localhost/db?motherduck.application_name=&&");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre(driver));
}

// A token must not come back in an error, nor must the scheme the driver hides.
TEST_F(MotherDuckConnectParamsTest, ErrorsOmitTokens) {
  // Rejected when the URI is set.
  for (const char* uri :
       {"motherduck:///db?my_secret_token",
        "motherduck:///db?motherduck.region=us-east-1&SECRET%40TOKEN",
        "motherduck:///db?motherduck.connect_timeout=SECRET_TOKEN",
        // A token where a port goes, for want of an '@': libpq would quote it.
        "motherduck://alice:SECRET_TOKEN/db", "motherduck://host:SECRET_TOKEN",
        "motherduck://h1:1,h2:SECRET_TOKEN/db", "md:SECRET_TOKEN@my_db"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(error_.message,
                ::testing::Not(::testing::AnyOf(::testing::HasSubstr("ret_token"),
                                                ::testing::HasSubstr("SECRET"),
                                                ::testing::HasSubstr("elsewhere"))));
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
  // libpq can't parse these. Its message quotes the URI, so isn't used.
  for (const char* uri : {"motherduck://alice:MY_SECRET_TOKEN@[bad",
                          "motherduck://alice:MY_SECRET_TOKEN%2@localhost/db"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(error_.message,
                ::testing::Not(::testing::AnyOf(::testing::HasSubstr("SECRET"),
                                                ::testing::HasSubstr("postgresql"))));
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
}

TEST(MotherDuckScrubConnectErrorTest, HidesTokenAndNonDefaultUser) {
  const std::string message =
      "connection failed: password authentication failed for user \"TOKEN123\" "
      "(TOKEN123) with PASS456";
  // A token in the user's place, and the password, are hidden everywhere.
  EXPECT_EQ(adbcpq::ScrubConnectError(message, "TOKEN123", "PASS456"),
            "connection failed: password authentication failed for user "
            "\"<redacted>\" (<redacted>) with <redacted>");
  // The default user isn't a secret.
  EXPECT_EQ(adbcpq::ScrubConnectError("failed for user \"postgres\"", "postgres", ""),
            "failed for user \"postgres\"");
  EXPECT_EQ(adbcpq::ScrubConnectError("a b", "", ""), "a b");
  // A user that matches part of the marker doesn't change what was replaced, and
  // the longer of two secrets at the same place wins.
  EXPECT_EQ(adbcpq::ScrubConnectError("pw red", "red", "pw"), "<redacted> <redacted>");
  EXPECT_EQ(adbcpq::ScrubConnectError("tokenX", "token", "tokenX"), "<redacted>");
  // A secret at either end of the message, twice in a row, and as both user and
  // password.
  EXPECT_EQ(adbcpq::ScrubConnectError("TOKEN failed", "TOKEN", ""), "<redacted> failed");
  EXPECT_EQ(adbcpq::ScrubConnectError("failed TOKEN", "TOKEN", ""), "failed <redacted>");
  EXPECT_EQ(adbcpq::ScrubConnectError("TOKEN", "", "TOKEN"), "<redacted>");
  EXPECT_EQ(adbcpq::ScrubConnectError("tokentoken", "token", ""), "<redacted>");
  EXPECT_EQ(adbcpq::ScrubConnectError("a TOKEN b", "TOKEN", "TOKEN"), "a <redacted> b");
  // Secrets that overlap leave nothing of either.
  EXPECT_EQ(adbcpq::ScrubConnectError("for xab here", "xa", "ab"), "for <redacted> here");
  EXPECT_EQ(adbcpq::ScrubConnectError("for xab here", "ab", "xa"), "for <redacted> here");
}

// libpq reads a password up to the '@' that ends the user info, whatever it holds,
// so a '?' in a password isn't the start of the query.
TEST_F(MotherDuckConnectParamsTest, QuestionMarkInPassword) {
  SetOption(ADBC_OPTION_PASSWORD, "");
  // What looks like an option is part of the password, and sets nothing.
  SetOption("uri",
            "motherduck://alice:eyJhbGci?motherduck.tls_root_cert_path=rest.of.token@"
            "db.example/analytics");
  EXPECT_THAT(Values("password"),
              ::testing::ElementsAre("eyJhbGci?motherduck.tls_root_cert_path="
                                     "rest.of.token"));
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("db.example"));
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("analytics"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  // A password with a '?' and then a real query.
  SetOption("uri",
            "motherduck://alice:p?ss@pg.us-east-1-aws.motherduck.com/analytics?"
            "motherduck.session_name=s");
  EXPECT_THAT(Values("password"), ::testing::ElementsAre("p?ss"));
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=single "
                                     "--session_name=s"));
  // The same, with the real query setting the region.
  SetOption("uri", "motherduck://alice:p?ss@/analytics?motherduck.region=us-east-1");
  EXPECT_THAT(Values("password"), ::testing::ElementsAre("p?ss"));
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  // Without user info, the first '?' still starts the query.
  EXPECT_EQ(database_.SetOption("uri", "motherduck://host/db?SECRET", &error_),
            ADBC_STATUS_INVALID_ARGUMENT);
  if (error_.release) error_.release(&error_);
  error_ = {};
}

// A query value with an '@' must not turn the text before the '?' into user info,
// which would send the token to the host after the '@'.
TEST_F(MotherDuckConnectParamsTest, AtSignInQueryValue) {
  SetOption(ADBC_OPTION_PASSWORD, "");
  const char* kSession =
      "--return_pg_command_tags=on --attach_mode=single "
      "--session_name=analyst@other.example";
  for (const char* uri :
       {"motherduck://expected.example?motherduck.session_name=analyst@other.example",
        "motherduck://"
        "expected.example:5432?motherduck.session_name=analyst@other.example",
        "motherduck://expected.example/db?motherduck.session_name=analyst@other.example",
        // A password that could be a port is read as one.
        "motherduck://"
        "expected.example:1234?motherduck.session_name=analyst@other.example"}) {
    SCOPED_TRACE(uri);
    SetOption("uri", uri);
    EXPECT_THAT(Values("host"), ::testing::ElementsAre("expected.example"));
    EXPECT_THAT(Values("options"), ::testing::ElementsAre(kSession));
    // Neither a user from the query nor the default one changes.
    EXPECT_THAT(Values("user"), ::testing::ElementsAre("postgres"));
  }
  SetOption("uri", "motherduck://[::1]?motherduck.session_name=analyst@other.example");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("::1"));
  EXPECT_THAT(Values("options"), ::testing::ElementsAre(kSession));
  // Ambiguous input, where the text before the '?' could be a host and port, is a
  // query; at worst it fails, and doesn't send the token anywhere else.
  for (const char* uri :
       {"motherduck://tok?en@host/db", "motherduck://alice:1234?x@host/db",
        "motherduck://alice?x=y@host/db"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_INVALID_ARGUMENT);
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
}

TEST_F(MotherDuckConnectParamsTest, Ports) {
  for (const char* uri : {"motherduck://host:5432/db", "motherduck://host/db",
                          "motherduck://h1:1,h2:2/db", "motherduck://h1,h2:2/db"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_OK);
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
  for (const char* uri : {"motherduck://host:54a2/db", "motherduck://host:%2B1/db"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_INVALID_ARGUMENT);
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
}

TEST_F(MotherDuckConnectParamsTest, UriQueryRejectsOtherParameters) {
  SetOption("uri", "motherduck://alice@localhost/db?motherduck.connect_timeout=4");
  // libpq's own parameters, PostgreSQL's names for supported settings, and
  // unknown parameters are all rejected, and the previous URI stays.
  for (const char* query :
       {"sslmode=verify-full", "sslrootcert=system", "options=-c%20x%3Dy", "service=md",
        "hostaddr=127.0.0.1", "host=elsewhere", "user=bob", "password=secret",
        "dbname=other", "application_name=dbt", "fallback_application_name=dbt",
        "connect_timeout=3", "requiressl=1", "motherduck.token=secret",
        "motherduck.unknown=1", "unknown", "adbc.motherduck.region=us-east-1",
        "motherduck.region", "motherduck.connect_timeout=1&sslmode=require",
        // %00 would end the C string a value is checked as.
        "motherduck.tls_verify=%00invalid", "motherduck.connect_timeout=10%00garbage",
        "motherduck.tls_verify%00extra=false", "motherduck.tls_verify=%00"}) {
    SCOPED_TRACE(query);
    const std::string uri = std::string("motherduck://bob@elsewhere/db?") + query;
    EXPECT_EQ(database_.SetOption("uri", uri.c_str(), &error_),
              ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_NE(error_.message, nullptr);
    // The message doesn't repeat values, which may hold a token.
    EXPECT_THAT(error_.message, ::testing::Not(::testing::HasSubstr("secret")));
    EXPECT_THAT(error_.message, ::testing::Not(::testing::HasSubstr("elsewhere")));
    if (error_.release) error_.release(&error_);
    error_ = {};
    EXPECT_THAT(Values("user"), ::testing::ElementsAre("alice"));
    EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("4"));
  }
  // The error says what the query takes, without naming the parameter, which may
  // be part of a token.
  EXPECT_EQ(database_.SetOption("uri", "md:db?sslmode=require", &error_),
            ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_THAT(error_.message,
              ::testing::AllOf(::testing::HasSubstr("unsupported query parameter"),
                               ::testing::HasSubstr(ADBC_MOTHERDUCK_OPTION_REGION),
                               ::testing::Not(::testing::HasSubstr("sslmode"))));
  if (error_.release) error_.release(&error_);
  error_ = {};
  // Invalid values and percent-encoding are rejected too.
  for (const char* query :
       {"motherduck.connect_timeout=ten", "motherduck.attach_mode=both",
        "motherduck.region=Not%20A%20Region", "motherduck.tls_verify=maybe"}) {
    SCOPED_TRACE(query);
    const std::string uri = std::string("motherduck:///db?") + query;
    EXPECT_EQ(database_.SetOption("uri", uri.c_str(), &error_),
              ADBC_STATUS_INVALID_ARGUMENT);
    // It says what the option expects, without repeating the value.
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(error_.message, ::testing::HasSubstr("expected"));
    EXPECT_THAT(error_.message,
                ::testing::Not(::testing::AnyOf(
                    ::testing::HasSubstr("ten"), ::testing::HasSubstr("both"),
                    ::testing::HasSubstr("Not%20"), ::testing::HasSubstr("maybe"))));
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
  for (const char* query : {"motherduck.region=%zz", "motherduck.region=%4"}) {
    SCOPED_TRACE(query);
    const std::string uri = std::string("motherduck:///db?") + query;
    EXPECT_EQ(database_.SetOption("uri", uri.c_str(), &error_),
              ADBC_STATUS_INVALID_ARGUMENT);
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("4"));
  // Unsetting the URI clears its settings.
  SetOption("uri", "");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_STATE);
}

TEST_F(MotherDuckConnectParamsTest, ShortFormDatabaseNames) {
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  // '@' and '/' after the '?' belong to the query, whose values may hold them.
  SetOption("uri", "md:my_db?motherduck.application_name=a%40b");
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("my_db"));
  SetOption("uri", "md:my_db?motherduck.session_name=a/b@c");
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("my_db"));
  // An encoded ':' is the workspace's own spelling.
  SetOption("uri", "md:md%3A");
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("md:"));
  // '@' and '/' in the database name are rejected in every spelling, since the
  // name would reach the server, which echoes it.
  for (const char* uri :
       {"md:SECRET@db", "md:SECRET%40db", "md:SECRET%40", "md:a%2Fb", "md:a%2fb",
        "md:a/b", "md:%00db", "md:db%00", "md:%zz", "md:%4", "md:%"}) {
    SCOPED_TRACE(uri);
    EXPECT_EQ(database_.SetOption("uri", uri, &error_), ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(error_.message, ::testing::Not(::testing::HasSubstr("SECRET")));
    if (error_.release) error_.release(&error_);
    error_ = {};
  }
  // The rejected ones left the last good URI in place.
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("md:"));
}

TEST_F(MotherDuckConnectParamsTest, RegionConflictsWithUriHost) {
  SetOption("uri", "motherduck://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_EQ(ADBC_STATUS_INVALID_ARGUMENT, Status());
}

TEST_F(MotherDuckConnectParamsTest, RegionWithoutUriHost) {
  SetOption("uri", "motherduck:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  EXPECT_THAT(Values("dbname"), ::testing::ElementsAre("db"));
}

TEST_F(MotherDuckConnectParamsTest, UsernameReplacesUriUser) {
  SetOption("uri", "motherduck://alice@localhost/db");
  SetOption(ADBC_OPTION_USERNAME, "bob");
  EXPECT_THAT(Values("user"), ::testing::ElementsAre("bob"));
}

TEST_F(MotherDuckConnectParamsTest, SessionOptions) {
  // The defaults go first, then PGOPTIONS, then explicit options.
  ScopedEnv pgoptions("PGOPTIONS", "-c x=y");
  SetOption("uri", "motherduck://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "adbc_session");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=single "
                                     "-c x=y --session_name=adbc_session"));
  SetOption(ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, "workspace");
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre("--return_pg_command_tags=on -c x=y --attach_mode=workspace "
                             "--session_name=adbc_session"));
}

TEST_F(MotherDuckConnectParamsTest, PgoptionsBetweenDefaultsAndOptions) {
  ScopedEnv pgoptions("PGOPTIONS", "--attach_mode=workspace");
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre(
          "--return_pg_command_tags=on --attach_mode=single --attach_mode=workspace"));
  SetOption(ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, "single");
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=workspace "
                             "--attach_mode=single"));
}

TEST_F(MotherDuckConnectParamsTest, SessionNameEscaped) {
  SetOption("uri", "motherduck://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "my session");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=single "
                                     "--session_name=my\\ session"));
  // Every ASCII whitespace character and backslash is escaped, and a byte of a
  // multi-byte character (0xA0 ends a no-break space) is left alone.
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
            "a\tb\nc\\d\xC2\xA0"
            "e");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=single "
                                     "--session_name=a\\\tb\\\nc\\\\d\xC2\xA0"
                                     "e"));
}

TEST_F(MotherDuckConnectParamsTest, ApplicationName) {
  const std::string driver = "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre(driver));
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "dbt");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("dbt " + driver));
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "");
  // PGAPPNAME is never used.
  ScopedEnv pgappname("PGAPPNAME", "env");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre(driver));
}

TEST_F(MotherDuckConnectParamsTest, ConnectTimeoutOption) {
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("connect_timeout"), ::testing::IsEmpty());
  SetOption(ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "10");
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("10"));
  // An explicit 0 is passed on, unlike an unset option.
  SetOption(ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "0");
  EXPECT_THAT(Values("connect_timeout"), ::testing::ElementsAre("0"));
  SetOption(ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT, "");
  EXPECT_THAT(Values("connect_timeout"), ::testing::IsEmpty());
}

TEST_F(MotherDuckConnectParamsTest, LongApplicationNameKeepsDriverName) {
  const std::string driver = "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  // PostgreSQL cuts names at 63 bytes, so a long name is shortened to leave the
  // driver's identifier whole.
  SetOption("uri", "motherduck://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, std::string(200, 'a').c_str());
  std::vector<std::string> names = Values("application_name");
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0].size(), 63u);
  EXPECT_THAT(names[0], ::testing::EndsWith(" " + driver));
  // A multi-byte character isn't cut in the middle, and a whole one that ends at
  // the limit stays: the name keeps every character that fits.
  const size_t budget = 63 - driver.size() - 1;
  std::string wide;
  for (int i = 0; i < 100; i++) wide += "\xC3\xA9";
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, wide.c_str());
  names = Values("application_name");
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], wide.substr(0, budget / 2 * 2) + " " + driver);
  // Three-byte characters too.
  std::string wider;
  for (int i = 0; i < 100; i++) wider += "\xE2\x82\xAC";
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, wider.c_str());
  names = Values("application_name");
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], wider.substr(0, budget / 3 * 3) + " " + driver);
  // A name that fits is unchanged.
  SetOption(ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME, "dbt");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("dbt " + driver));
}

TEST_F(MotherDuckConnectParamsTest, RegionVerifiesServer) {
  SetOption("uri", "motherduck:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
}

TEST_F(MotherDuckConnectParamsTest, VerifiesServer) {
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  EXPECT_EQ(stderr_, "");
}

TEST_F(MotherDuckConnectParamsTest, NoCaBundleFailsClosed) {
  // Without root certificates, connecting fails rather than falling back to an
  // unverified connection, and the error says how to fix it.
  SetCaBundle("");
  SetOption("uri", "motherduck://localhost/db");
  for (const char* pgsslmode : {static_cast<const char*>(nullptr), "verify-full"}) {
    ScopedEnv env("PGSSLMODE", pgsslmode);
    std::vector<std::pair<std::string, std::string>> params;
    EXPECT_THAT(database_.ConnectParams(&params, &error_),
                IsStatus(ADBC_STATUS_INVALID_STATE, &error_));
    ASSERT_NE(error_.message, nullptr);
    EXPECT_THAT(
        error_.message,
        ::testing::AllOf(::testing::HasSubstr("No trusted CA certificates"),
                         ::testing::HasSubstr(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH),
                         ::testing::HasSubstr("SSL_CERT_FILE"),
                         ::testing::HasSubstr(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY)));
    if (error_.release) error_.release(&error_);
  }
  // With a region too.
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  SetOption("uri", "motherduck:///db");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_STATE);
  // Modes that don't verify, root certificates the connection names, and
  // tls_verify=false don't need a found bundle.
  {
    ScopedEnv pgsslmode("PGSSLMODE", "require");
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  }
  {
    ScopedEnv pgsslrootcert("PGSSLROOTCERT", "/tmp/root.crt");
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  }
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "false");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  EXPECT_EQ(stderr_, "");
}

TEST_F(MotherDuckConnectParamsTest, UnreadableSslCertFileFails) {
  // A bundle SSL_CERT_FILE names but that can't be read is reported, even when
  // the driver could find another.
  ScopedEnv ssl_cert_file("SSL_CERT_FILE", "/nonexistent/motherduck-test-ca.pem");
  SetOption("uri", "motherduck://localhost/db");
  std::vector<std::pair<std::string, std::string>> params;
  EXPECT_THAT(database_.ConnectParams(&params, &error_),
              IsStatus(ADBC_STATUS_INVALID_STATE, &error_));
  ASSERT_NE(error_.message, nullptr);
  EXPECT_THAT(error_.message,
              ::testing::HasSubstr("/nonexistent/motherduck-test-ca.pem"));
  if (error_.release) error_.release(&error_);
  // It isn't consulted when the connection names root certificates.
  ScopedEnv pgsslrootcert("PGSSLROOTCERT", "/tmp/root.crt");
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre("/tmp/root.crt"));
}

TEST_F(MotherDuckConnectParamsTest, EmptyEnvironmentVerifiesServer) {
  // An empty PGSSLMODE or PGSSLROOTCERT is unset.
  ScopedEnv pgsslmode("PGSSLMODE", "");
  ScopedEnv pgsslrootcert("PGSSLROOTCERT", "");
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
}

TEST_F(MotherDuckConnectParamsTest, CaBundleForVerifyingModes) {
  // An explicit verify-full gets the bundle too, so it works without
  // sslrootcert.
  SetOption("uri", "motherduck://localhost/db");
  {
    ScopedEnv pgsslmode("PGSSLMODE", "verify-full");
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
    EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  }
  // verify-ca doesn't check the host name, so every public CA would accept any
  // public certificate for any host. It keeps libpq's behavior: it needs root
  // certificates the user chose.
  {
    ScopedEnv pgsslmode("PGSSLMODE", "verify-ca");
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-ca"));
    EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  }
  // A root file would make the other modes verify the server's CA.
  for (const char* sslmode : {"disable", "allow", "prefer", "require"}) {
    ScopedEnv pgsslmode("PGSSLMODE", sslmode);
    EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  }
}

TEST(MotherDuckTlsOptionsTest, OptionSetGet) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);
  ASSERT_THAT(
      AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH,
                            "/etc/roots.pem", &error),
      IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, &error),
              ::testing::Optional("/etc/roots.pem"s));
  // An empty value unsets it.
  ASSERT_THAT(AdbcDatabaseSetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(
      GetOptionStatus(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, &error),
      IsStatus(ADBC_STATUS_NOT_FOUND, &error));
  if (error.release) error.release(&error);

  // Verification is on by default.
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, &error),
              ::testing::Optional("true"s));
  for (const std::string value : {"false", "true"}) {
    ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY,
                                      value.c_str(), &error),
                IsOkStatus(&error));
    EXPECT_THAT(adbc_validation::DatabaseGetOption(
                    &database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, &error),
                ::testing::Optional(value));
  }
  for (const char* value : {"no", "FALSE", "0"}) {
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY,
                                      value, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  // An empty value unsets it, which reads back as the default.
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY,
                                    "false", &error),
              IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY,
                                    "", &error),
              IsOkStatus(&error));
  EXPECT_THAT(adbc_validation::DatabaseGetOption(
                  &database.value, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, &error),
              ::testing::Optional("true"s));
}

// A PEM file for motherduck.tls_root_cert_path, removed afterwards.
class MotherDuckTlsRootCertPathTest : public MotherDuckConnectParamsTest {
 protected:
  void SetUp() override {
    MotherDuckConnectParamsTest::SetUp();
    dir_ = std::filesystem::temp_directory_path() /
           ("motherduck_test_roots_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::create_directories(dir_);
    roots_ = (dir_ / "roots.pem").string();
    std::ofstream(roots_) << "not checked\n";
  }
  void TearDown() override {
    std::filesystem::remove_all(dir_);
    MotherDuckConnectParamsTest::TearDown();
  }

  std::filesystem::path dir_;
  std::string roots_;
};

TEST_F(MotherDuckTlsRootCertPathTest, VerifiesWithTheFile) {
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, roots_.c_str());
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(roots_));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  // It outranks the environment and a connection service, so it alone is passed.
  {
    ScopedEnv pgsslmode("PGSSLMODE", "require");
    ScopedEnv pgsslrootcert("PGSSLROOTCERT", "/tmp/other.crt");
    ScopedEnv pgservice("PGSERVICE", "md");
    EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(roots_));
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  }
  // So does a region.
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  SetOption("uri", "motherduck:///db");
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(roots_));
}

TEST_F(MotherDuckTlsRootCertPathTest, FromUriQuery) {
  SetOption(
      "uri",
      ("motherduck://localhost/db?motherduck.tls_root_cert_path=" + roots_).c_str());
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(roots_));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
}

TEST_F(MotherDuckTlsRootCertPathTest, Conflicts) {
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, roots_.c_str());
  SetOption("uri", "motherduck://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "false");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  // The URI can set either one: the root certificates conflict with the option,
  // and the option outranks the URI's tls_verify.
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, "");
  SetOption(
      "uri",
      ("motherduck://localhost/db?motherduck.tls_root_cert_path=" + roots_).c_str());
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "true");
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
  SetOption("uri", ("motherduck://localhost/db?motherduck.tls_root_cert_path=" + roots_ +
                    "&motherduck.tls_verify=false")
                       .c_str());
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
  // Unset, the URI's value applies again.
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
}

TEST_F(MotherDuckTlsRootCertPathTest, UnreadableFile) {
  SetOption("uri", "motherduck://localhost/db");
  for (const std::string& path : {(dir_ / "missing.pem").string(), dir_.string()}) {
    SCOPED_TRACE(path);
    SetOption(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, path.c_str());
    std::vector<std::pair<std::string, std::string>> params;
    EXPECT_THAT(database_.ConnectParams(&params, &error_),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error_));
    EXPECT_THAT(error_.message,
                ::testing::HasSubstr(ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH));
    if (error_.release) error_.release(&error_);
  }
}

TEST_F(MotherDuckConnectParamsTest, TlsVerifyFalse) {
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "false");
  // Encrypted, but nothing to verify with: no found bundle, no warning.
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  EXPECT_EQ(stderr_, "");
  EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  // It outranks the environment and a connection service.
  {
    ScopedEnv pgsslmode("PGSSLMODE", "verify-full");
    ScopedEnv pgsslrootcert("PGSSLROOTCERT", "/tmp/other.crt");
    ScopedEnv pgservice("PGSERVICE", "md");
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
    EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  }
  // true restores the default.
  SetOption(ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, "true");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
}

TEST_F(MotherDuckConnectParamsTest, DefaultRootCertFile) {
  // libpq's own default, ~/.postgresql/root.crt, wins over the found bundle.
  std::filesystem::path home =
      std::filesystem::temp_directory_path() /
      ("motherduck_test_home_" +
       std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
#ifdef _WIN32
  std::filesystem::path root_crt = home / "postgresql" / "root.crt";
  ScopedEnv appdata("APPDATA", home.string().c_str());
#else
  std::filesystem::path root_crt = home / ".postgresql" / "root.crt";
  ScopedEnv home_env("HOME", home.string().c_str());
#endif
  std::filesystem::create_directories(root_crt.parent_path());
  std::ofstream(root_crt) << "not checked\n";
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  std::filesystem::remove_all(home);
}

TEST_F(MotherDuckConnectParamsTest, KeepsPgsslmode) {
  ScopedEnv pgsslmode("PGSSLMODE", "verify-full");
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  // A verifying mode gets the found bundle, but only once PGSSLROOTCERT, passed
  // explicitly, doesn't name root certificates.
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  ScopedEnv pgsslrootcert("PGSSLROOTCERT", "/etc/ca.pem");
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre("/etc/ca.pem"));
}

TEST_F(MotherDuckConnectParamsTest, KeepsServiceSslmode) {
  // The service file may set sslmode and sslrootcert, which defaults here
  // would override.
  ScopedEnv pgservice("PGSERVICE", "md");
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::IsEmpty());
}

// Every combination of region, PGSSLMODE, PGSSLROOTCERT, and whether the driver
// finds a CA bundle. When PGSSLMODE is unset and PGSSLROOTCERT isn't system
// (which already makes libpq default to verify-full), the driver adds
// verify-full. It adds the bundle it found when nothing names root certificates
// and the mode is unset or verify-full, and fails when it needs one and has none.
TEST_F(MotherDuckConnectParamsTest, SslmodeMatrix) {
  struct RootCert {
    const char* env;
    bool system;
  };
  const RootCert kRootCerts[] = {
      {nullptr, false},
      {"system", true},
      {"/tmp/root.crt", false},
  };
  for (const char* region : {"", "us-east-1"}) {
    for (const char* env_mode : {static_cast<const char*>(nullptr), "disable", "allow",
                                 "prefer", "require", "verify-ca", "verify-full"}) {
      for (const RootCert& rootcert : kRootCerts) {
        for (const char* bundle : {kBundle, ""}) {
          ScopedEnv pgsslmode("PGSSLMODE", env_mode);
          ScopedEnv pgsslrootcert("PGSSLROOTCERT", rootcert.env);
          SetCaBundle(bundle);
          SCOPED_TRACE(std::string("region=") + region +
                       " PGSSLMODE=" + (env_mode ? env_mode : "(unset)") +
                       " PGSSLROOTCERT=" + (rootcert.env ? rootcert.env : "(unset)") +
                       " bundle=" + bundle);
          SetOption("uri", *region != '\0' ? "motherduck:///db" : "motherduck://h/db");
          SetOption(ADBC_MOTHERDUCK_OPTION_REGION, region);

          std::string mode = env_mode ? env_mode : "";
          bool named_roots = rootcert.env != nullptr;
          if (!named_roots && *bundle == '\0' &&
              (mode.empty() || mode == "verify-full")) {
            EXPECT_EQ(Status(), ADBC_STATUS_INVALID_STATE);
            continue;
          }
          // Only modes that check the host name get the found bundle.
          bool add_bundle =
              !named_roots && *bundle != '\0' && (mode.empty() || mode == "verify-full");
          // PGSSLMODE and PGSSLROOTCERT are passed explicitly, so libpq sees what
          // the driver saw.
          std::vector<std::string> expected_rootcert;
          if (rootcert.env != nullptr) expected_rootcert.push_back(rootcert.env);
          if (add_bundle) expected_rootcert.push_back(bundle);
          std::vector<std::string> expected_mode;
          if (env_mode != nullptr) {
            expected_mode.push_back(env_mode);
          } else if (!rootcert.system) {
            expected_mode.push_back("verify-full");
          }
          EXPECT_EQ(Values("sslmode"), expected_mode);
          EXPECT_EQ(Values("sslrootcert"), expected_rootcert);
        }
      }
    }
  }
}

TEST_F(MotherDuckConnectParamsTest, RegionPort) {
  SetOption("uri", "motherduck:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("5432"));
  SetOption("uri", "motherduck://:6543/db");
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("6543"));
}

TEST_F(MotherDuckConnectParamsTest, RegionMatchingHost) {
  SetOption("uri", "motherduck://pg.us-east-1-aws.motherduck.com/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  // Hostnames ignore case and a trailing dot.
  SetOption("uri", "motherduck://PG.US-EAST-1-AWS.MOTHERDUCK.COM./db");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("PG.US-EAST-1-AWS.MOTHERDUCK.COM."));
}

TEST_F(MotherDuckConnectParamsTest, RegionLowercased) {
  SetOption("uri", "motherduck:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "US-East-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  char region[32];
  size_t length = sizeof(region);
  ASSERT_THAT(
      database_.GetOption(ADBC_MOTHERDUCK_OPTION_REGION, region, &length, &error_),
      IsOkStatus(&error_));
  EXPECT_STREQ(region, "us-east-1");
}

TEST_F(MotherDuckConnectParamsTest, RegionRejectsService) {
  // libpq would fill hostaddr from the service, connecting elsewhere.
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  SetOption("uri", "motherduck:///db");
  {
    ScopedEnv pgservice("PGSERVICE", "local");
    EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  }
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
}

TEST_F(MotherDuckConnectParamsTest, OptionsLoneBackslash) {
  // With nothing appended after them, a trailing backslash is harmless.
  ScopedEnv pgoptions("PGOPTIONS", "-c x=y\\");
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
  // It would escape the space before an explicit option.
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "s");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  // An escaped backslash is fine.
  ScopedEnv escaped("PGOPTIONS", "-c x=y\\\\");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--return_pg_command_tags=on --attach_mode=single "
                                     "-c x=y\\\\ --session_name=s"));
}

TEST_F(MotherDuckConnectParamsTest, NoRegionLeavesPort) {
  SetOption("uri", "motherduck://localhost/db");
  EXPECT_THAT(Values("port"), ::testing::IsEmpty());
}

// A NULL option value used to be dereferenced (strcmp, std::string).
TEST(MotherDuckNullOptionTest, DatabaseRejectsNullValue) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  for (const char* key :
       {"uri", ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
        ADBC_MOTHERDUCK_OPTION_REGION, ADBC_MOTHERDUCK_OPTION_TOKEN, ADBC_OPTION_USERNAME,
        ADBC_OPTION_PASSWORD, "adbc.postgresql.type_resolver_mode",
        "adbc.postgresql.use_copy", "adbc.unknown_option"}) {
    SCOPED_TRACE(key);
    EXPECT_THAT(AdbcDatabaseSetOption(&database.value, key, nullptr, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
}

TEST(MotherDuckNullOptionTest, ConnectionRejectsNullValue) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcConnection> connection;
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  for (const char* key :
       {ADBC_CONNECTION_OPTION_AUTOCOMMIT, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
        ADBC_CONNECTION_OPTION_ISOLATION_LEVEL, "adbc.postgresql.use_copy"}) {
    SCOPED_TRACE(key);
    EXPECT_THAT(AdbcConnectionSetOption(&connection.value, key, nullptr, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
}

TEST(MotherDuckNullOptionTest, StatementRejectsNullValue) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcConnection> connection;
  adbc_validation::Handle<struct AdbcStatement> statement;
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  for (const char* key :
       {ADBC_INGEST_OPTION_TARGET_TABLE, ADBC_INGEST_OPTION_MODE,
        ADBC_INGEST_OPTION_TEMPORARY, "adbc.postgresql.batch_size_hint_bytes",
        "adbc.postgresql.use_copy", "adbc.unknown_option"}) {
    SCOPED_TRACE(key);
    EXPECT_THAT(AdbcStatementSetOption(&statement.value, key, nullptr, &error),
                IsStatus(ADBC_STATUS_INVALID_ARGUMENT, &error));
    if (error.release) error.release(&error);
  }
  // NULL clears the target schema.
  EXPECT_THAT(AdbcStatementSetOption(
                  &statement.value, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, nullptr, &error),
              IsOkStatus(&error));
}

// The Postgres endpoint can't create temporary tables, so enabling temporary
// ingestion fails up front rather than at CREATE TEMP TABLE.
TEST(MotherDuckIngestOptionTest, TemporaryNotSupported) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcConnection> connection;
  adbc_validation::Handle<struct AdbcStatement> statement;
  ASSERT_THAT(AdbcConnectionNew(&connection.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcStatementNew(&connection.value, &statement.value, &error),
              IsOkStatus(&error));
  EXPECT_THAT(AdbcStatementSetOption(&statement.value, ADBC_INGEST_OPTION_TEMPORARY,
                                     ADBC_OPTION_VALUE_ENABLED, &error),
              IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error));
  EXPECT_THAT(error.message ? error.message : "",
              ::testing::HasSubstr("temporary table"));
  if (error.release) error.release(&error);
  // Disabling it, as clients such as Python always do, still works.
  EXPECT_THAT(AdbcStatementSetOption(&statement.value, ADBC_INGEST_OPTION_TEMPORARY,
                                     ADBC_OPTION_VALUE_DISABLED, &error),
              IsOkStatus(&error));
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
