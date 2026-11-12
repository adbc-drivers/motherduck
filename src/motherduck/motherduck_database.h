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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "auth/token_cache.h"
#include "database.h"

#define ADBC_MOTHERDUCK_OPTION_ATTACH_MODE "adbc.motherduck.attach_mode"
#define ADBC_MOTHERDUCK_OPTION_SESSION_NAME "adbc.motherduck.session_name"
#define ADBC_MOTHERDUCK_OPTION_REGION "adbc.motherduck.region"
/// An alias for the standard ADBC password option, which holds the token.
#define ADBC_MOTHERDUCK_OPTION_TOKEN "adbc.motherduck.token"
#define ADBC_MOTHERDUCK_OPTION_AUTH_TYPE "adbc.motherduck.auth_type"
#define ADBC_MOTHERDUCK_OPTION_OAUTH_CLIENT_ID "adbc.motherduck.oauth_client_id"
#define ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT "adbc.motherduck.login_timeout"

namespace adbcpq {

/// \brief The Postgres endpoint host for a MotherDuck region, e.g.
/// pg.us-east-1-aws.motherduck.com for us-east-1 or
/// pg.eastus2-azure.motherduck.com for eastus2, or empty if region can't be
/// part of a hostname.
std::string RegionHost(std::string_view region);

/// \brief Whether a connection should default to sslmode=require: neither the
/// URI nor PGSSLMODE sets sslmode, and sslrootcert (from the URI, or
/// PGSSLROOTCERT) isn't system, which makes libpq default to verify-full.
bool DefaultsToRequireSsl(std::string_view uri_sslmode, std::string_view uri_sslrootcert);

/// \brief A libpq session option, --<name>=<value>, with whitespace and
/// backslashes in value escaped.
std::string SessionOption(std::string_view name, std::string_view value);

/// \brief libpq connection parameters, as keyword/value pairs.
using LibpqParams = std::vector<std::pair<std::string, std::string>>;

/// \brief Whether a libpq connection error may mean the endpoint rejected the
///   token, so it's worth asking the MotherDuck API.
///
/// libpq drops the SQLSTATE of errors raised while connecting, so this matches
/// the messages of MotherDuck's Postgres endpoint.
bool IsAuthFailure(std::string_view message);

/// \brief The parameters with browser login's token as the password and, if
///   need_host, the organization's endpoint as the host.
LibpqParams AddCredentials(LibpqParams params, const auth::Credentials& credentials,
                           bool need_host);

class MotherduckDatabase : public PostgresDatabase {
 public:
  // The endpoint rejects binary COPY TO STDOUT, so results don't use it unless
  // adbc.postgresql.use_copy asks for it (#11).
  MotherduckDatabase() { use_copy_ = false; }

  AdbcStatusCode Init(struct AdbcError* error) override;
  AdbcStatusCode GetOption(const char* option, char* value, size_t* length,
                           struct AdbcError* error) override;
  AdbcStatusCode SetOption(const char* key, const char* value,
                           struct AdbcError* error) override;
  AdbcStatusCode GetOptionInt(const char* option, int64_t* value,
                              struct AdbcError* error) override;
  AdbcStatusCode SetOptionInt(const char* key, int64_t value,
                              struct AdbcError* error) override;
  AdbcStatusCode Connect(PGconn** conn, struct AdbcError* error) override;

  /// \brief The libpq keywords and values Connect passes, in order. With browser
  ///   login, these don't include the token, and they include the host only if
  ///   the URI, an environment variable or the region names one.
  AdbcStatusCode ConnectParams(std::vector<std::pair<std::string, std::string>>* params,
                               struct AdbcError* error) const;

  std::string_view InfoVendorName() override { return "MotherDuck"; }
  std::string_view DriverName() override { return "ADBC MotherDuck Driver"; }
  std::string_view DriverVersion() override;

 private:
  // Empty until set, so the URI can override the default.
  std::string attach_mode_;
  std::string session_name_;
  std::string region_;
  std::string username_;
  std::string password_;
  std::string auth_type_ = "token";
  std::string oauth_client_id_{auth::kDefaultClientId};
  int64_t login_timeout_seconds_ = 120;
  // Set by Init when auth_type is browser.
  std::unique_ptr<auth::TokenCache> token_cache_;
};

}  // namespace adbcpq
