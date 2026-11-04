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
  // It reports the user the driver connects as: postgres by default, then the
  // URI's user, then the option.
  EXPECT_THAT(
      adbc_validation::DatabaseGetOption(&database.value, ADBC_OPTION_USERNAME, &error),
      ::testing::Optional("postgres"s));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri",
                                    "postgresql://alice@localhost/db", &error),
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

TEST(MotherDuckUsernameTest, UnparseableUri) {
  struct AdbcError error = {};
  adbc_validation::Handle<struct AdbcDatabase> database;
  ASSERT_THAT(AdbcDatabaseNew(&database.value, &error), IsOkStatus(&error));
  ASSERT_THAT(AdbcDatabaseSetOption(&database.value, "uri", "postgresql://[bad", &error),
              IsOkStatus(&error));
  EXPECT_THAT(GetOptionStatus(&database.value, ADBC_OPTION_USERNAME, &error),
              IsStatus(ADBC_STATUS_INVALID_STATE, &error));
  if (error.release) error.release(&error);
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
  // Where libpq looks for its default root certificate, ~/.postgresql/root.crt
  ScopedEnv home_{"HOME", "/nonexistent"};
  ScopedEnv appdata_{"APPDATA", "C:\\nonexistent"};
  static constexpr char kBundle[] = "/found/ca-bundle.pem";
  adbcpq::MotherduckDatabase database_;
  struct AdbcError error_ = {};
  std::string stderr_;
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
  // The default attach mode goes first, so the URI can override it; explicit
  // options go last, so they override the URI.
  SetOption("uri", "postgresql://localhost/db?options=-c%20x%3Dy");
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "adbc_session");
  EXPECT_THAT(
      Values("options"),
      ::testing::ElementsAre("--attach_mode=single -c x=y --session_name=adbc_session"));
  SetOption(ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, "workspace");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre(
                  "-c x=y --attach_mode=workspace --session_name=adbc_session"));
}

TEST_F(MotherDuckConnectParamsTest, PgoptionsBetweenDefaultsAndOptions) {
  ScopedEnv pgoptions("PGOPTIONS", "--attach_mode=workspace");
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--attach_mode=single --attach_mode=workspace"));
  SetOption(ADBC_MOTHERDUCK_OPTION_ATTACH_MODE, "single");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--attach_mode=workspace --attach_mode=single"));
}

TEST_F(MotherDuckConnectParamsTest, SessionNameEscaped) {
  SetOption("uri", "postgresql://localhost/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "my session");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--attach_mode=single --session_name=my\\ session"));
}

TEST_F(MotherDuckConnectParamsTest, ApplicationName) {
  const std::string driver = "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre(driver));
  SetOption("uri", "postgresql://localhost/db?fallback_application_name=fallback");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("fallback " + driver));
  EXPECT_THAT(Values("fallback_application_name"), ::testing::IsEmpty());
  SetOption("uri", "postgresql://localhost/db?application_name=dbt");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre("dbt " + driver));
  // PGAPPNAME is never used.
  ScopedEnv pgappname("PGAPPNAME", "env");
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("application_name"), ::testing::ElementsAre(driver));
}

TEST_F(MotherDuckConnectParamsTest, RegionVerifiesServer) {
  SetOption("uri", "postgresql:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
}

TEST_F(MotherDuckConnectParamsTest, VerifiesServer) {
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  SetOption("uri", "host=localhost dbname=db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  EXPECT_EQ(stderr_, "");
}

TEST_F(MotherDuckConnectParamsTest, NoCaBundleRequiresTls) {
  SetCaBundle("");
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  // The warning comes once per process, so an earlier test may have had it.
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  SetOption("uri", "postgresql:///db");
  std::string printed = stderr_;
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
  printed += stderr_;
  EXPECT_THAT(printed, ::testing::AnyOf(
                           "", "[motherduck] warning: no trusted CA certificates found, "
                               "so the driver can't verify the server's certificate; "
                               "using sslmode=require. Set SSL_CERT_FILE, or "
                               "sslrootcert in the URI, to a PEM CA bundle\n"));
}

TEST_F(MotherDuckConnectParamsTest, EmptySslmodeVerifiesServer) {
  // libpq uses the last value given for a keyword, so verify-full wins.
  SetOption("uri", "postgresql://localhost/db?sslmode=");
  EXPECT_EQ(Values("sslmode").back(), "verify-full");
  SetOption("uri", "host=localhost dbname=db sslmode=''");
  EXPECT_EQ(Values("sslmode").back(), "verify-full");
  SetOption("uri", "postgresql://localhost/db?sslrootcert=");
  EXPECT_EQ(Values("sslrootcert").back(), kBundle);
  ScopedEnv pgsslmode("PGSSLMODE", "");
  ScopedEnv pgsslrootcert("PGSSLROOTCERT", "");
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
}

TEST_F(MotherDuckConnectParamsTest, CaBundleForVerifyingModes) {
  // An explicit verify-ca or verify-full gets the bundle too, so it works
  // without sslrootcert.
  for (const char* sslmode : {"verify-ca", "verify-full"}) {
    SetOption("uri", (std::string("postgresql://localhost/db?sslmode=") + sslmode).c_str());
    EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre(sslmode));
    EXPECT_THAT(Values("sslrootcert"), ::testing::ElementsAre(kBundle));
  }
  // A root file would make the other modes verify the server's CA.
  for (const char* sslmode : {"disable", "allow", "prefer", "require"}) {
    SetOption("uri", (std::string("postgresql://localhost/db?sslmode=") + sslmode).c_str());
    EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  }
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
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  EXPECT_THAT(Values("sslrootcert"), ::testing::IsEmpty());
  std::filesystem::remove_all(home);
}

TEST_F(MotherDuckConnectParamsTest, KeepsUriSslmode) {
  SetOption("uri", "postgresql://localhost/db?sslmode=verify-full");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("verify-full"));
  SetOption("uri", "host=localhost dbname=db sslmode=disable");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("disable"));
}

TEST_F(MotherDuckConnectParamsTest, KeepsRequiressl) {
  // libpq translates the deprecated requiressl=1 into sslmode=require.
  SetOption("uri", "host=localhost dbname=db requiressl=1");
  EXPECT_THAT(Values("sslmode"), ::testing::ElementsAre("require"));
}

TEST_F(MotherDuckConnectParamsTest, KeepsPgsslmode) {
  ScopedEnv pgsslmode("PGSSLMODE", "verify-full");
  SetOption("uri", "postgresql://localhost/db");
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
  SetOption("uri", "service=md dbname=db");
  EXPECT_THAT(Values("sslmode"), ::testing::IsEmpty());
  ScopedEnv pgservice("PGSERVICE", "md");
  SetOption("uri", "postgresql://localhost/db");
  EXPECT_THAT(Values("sslmode"), ::testing::IsEmpty());
}

// Every combination of region, sslmode (URI and PGSSLMODE), sslrootcert
// source, and whether the driver finds a CA bundle. The URI's sslmode and
// sslrootcert pass through unchanged. When nothing else sets sslmode and
// sslrootcert isn't system (which already makes libpq default to verify-full),
// the driver adds verify-full if it has root certificates, and otherwise
// require. It adds the bundle it found when nothing names root certificates
// and sslmode is unset or verifies.
TEST_F(MotherDuckConnectParamsTest, SslmodeMatrix) {
  struct RootCert {
    const char* uri;
    const char* env;
    bool system;
  };
  const RootCert kRootCerts[] = {
      {"", nullptr, false},
      {"system", nullptr, true},
      {"", "system", true},
      {"/tmp/root.crt", nullptr, false},
      {"", "/tmp/root.crt", false},
      // The URI wins over PGSSLROOTCERT.
      {"/tmp/root.crt", "system", false},
  };
  for (const char* region : {"", "us-east-1"}) {
    for (const char* uri_mode :
         {"", "disable", "allow", "prefer", "require", "verify-ca", "verify-full"}) {
      for (const char* env_mode :
           {static_cast<const char*>(nullptr), "prefer", "verify-full"}) {
        for (const RootCert& rootcert : kRootCerts) {
          for (const char* bundle : {kBundle, ""}) {
            ScopedEnv pgsslmode("PGSSLMODE", env_mode);
            ScopedEnv pgsslrootcert("PGSSLROOTCERT", rootcert.env);
            SetCaBundle(bundle);
            std::string query;
            if (*uri_mode != '\0') query += std::string("&sslmode=") + uri_mode;
            if (*rootcert.uri != '\0') {
              query += std::string("&sslrootcert=") + rootcert.uri;
            }
            std::string uri = "postgresql:///db";
            if (!query.empty()) uri += "?" + query.substr(1);
            SCOPED_TRACE(uri + " region=" + region +
                         " PGSSLMODE=" + (env_mode ? env_mode : "(unset)") +
                         " PGSSLROOTCERT=" + (rootcert.env ? rootcert.env : "(unset)") +
                         " bundle=" + bundle);
            SetOption("uri", uri.c_str());
            SetOption(ADBC_MOTHERDUCK_OPTION_REGION, region);

            std::string mode = *uri_mode != '\0' ? uri_mode : env_mode ? env_mode : "";
            bool named_roots = *rootcert.uri != '\0' || rootcert.env != nullptr;
            bool add_bundle = !named_roots && *bundle != '\0' &&
                              (mode.empty() || mode.rfind("verify-", 0) == 0);
            // The URI's settings pass through, and PGSSLMODE and PGSSLROOTCERT
            // are passed explicitly when the URI doesn't set them, so libpq sees
            // what the driver saw.
            std::vector<std::string> expected_rootcert;
            if (*rootcert.uri != '\0') {
              expected_rootcert.push_back(rootcert.uri);
            } else if (rootcert.env != nullptr) {
              expected_rootcert.push_back(rootcert.env);
            }
            if (add_bundle) expected_rootcert.push_back(bundle);
            std::vector<std::string> expected_mode;
            if (*uri_mode != '\0') {
              expected_mode.push_back(uri_mode);
            } else if (env_mode != nullptr) {
              expected_mode.push_back(env_mode);
            } else if (!rootcert.system) {
              expected_mode.push_back(named_roots || add_bundle ? "verify-full"
                                                                : "require");
            }
            EXPECT_EQ(Values("sslmode"), expected_mode);
            EXPECT_EQ(Values("sslrootcert"), expected_rootcert);
          }
        }
      }
    }
  }
}

TEST_F(MotherDuckConnectParamsTest, RegionPort) {
  SetOption("uri", "postgresql:///db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("5432"));
  SetOption("uri", "postgresql://:6543/db");
  EXPECT_THAT(Values("port"), ::testing::ElementsAre("6543"));
}

TEST_F(MotherDuckConnectParamsTest, RegionMatchingHost) {
  SetOption("uri", "postgresql://pg.us-east-1-aws.motherduck.com/db");
  SetOption(ADBC_MOTHERDUCK_OPTION_REGION, "us-east-1");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("pg.us-east-1-aws.motherduck.com"));
  // Hostnames ignore case and a trailing dot.
  SetOption("uri", "host=PG.US-EAST-1-AWS.MOTHERDUCK.COM. dbname=db");
  EXPECT_THAT(Values("host"), ::testing::ElementsAre("PG.US-EAST-1-AWS.MOTHERDUCK.COM."));
}

TEST_F(MotherDuckConnectParamsTest, RegionLowercased) {
  SetOption("uri", "postgresql:///db");
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
  SetOption("uri", "service=local dbname=db");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  SetOption("uri", "postgresql:///db");
  {
    ScopedEnv pgservice("PGSERVICE", "local");
    EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  }
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
}

TEST_F(MotherDuckConnectParamsTest, OptionsLoneBackslash) {
  // With nothing appended after them, a trailing backslash is harmless.
  SetOption("uri", "postgresql://localhost/db?options=-c%20x%3Dy%5C");
  EXPECT_EQ(Status(), ADBC_STATUS_OK);
  // It would escape the space before an explicit option.
  SetOption(ADBC_MOTHERDUCK_OPTION_SESSION_NAME, "s");
  EXPECT_EQ(Status(), ADBC_STATUS_INVALID_ARGUMENT);
  // An escaped backslash is fine.
  SetOption("uri", "postgresql://localhost/db?options=-c%20x%3Dy%5C%5C");
  EXPECT_THAT(Values("options"),
              ::testing::ElementsAre("--attach_mode=single -c x=y\\\\ --session_name=s"));
}

TEST_F(MotherDuckConnectParamsTest, NoRegionLeavesPort) {
  SetOption("uri", "postgresql://localhost/db");
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
