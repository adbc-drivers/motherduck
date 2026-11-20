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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "auth/token_cache.h"
#include "database.h"
#include "motherduck_trust_store.h"

#define ADBC_MOTHERDUCK_OPTION_ATTACH_MODE "motherduck.attach_mode"
#define ADBC_MOTHERDUCK_OPTION_SESSION_NAME "motherduck.session_name"
/// The application's name, which the driver puts in front of its own in MotherDuck's
/// user agent.
#define ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME "motherduck.application_name"
/// The longest time to wait while connecting, in seconds.
#define ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT "motherduck.connect_timeout"
#define ADBC_MOTHERDUCK_OPTION_REGION "motherduck.region"
/// The schema new connections start in, set with SET search_path.
#define ADBC_MOTHERDUCK_OPTION_DB_SCHEMA "motherduck.db_schema"
/// An alias for the standard ADBC password option, which holds the token.
#define ADBC_MOTHERDUCK_OPTION_TOKEN "motherduck.token"
/// A PEM file of root certificates to verify the server with, instead of the
/// ones the driver finds. Implies sslmode=verify-full.
#define ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH "motherduck.tls_root_cert_path"
/// "false" encrypts the connection without verifying the server's
/// certificate (sslmode=require). Defaults to "true".
#define ADBC_MOTHERDUCK_OPTION_TLS_VERIFY "motherduck.tls_verify"
/// How the driver gets the token: "token" (the default) or "browser", which logs in
/// through the browser.
#define ADBC_MOTHERDUCK_OPTION_AUTH_TYPE "motherduck.auth_type"
/// The Auth0 application browser login goes through.
#define ADBC_MOTHERDUCK_OPTION_OAUTH_CLIENT_ID "motherduck.oauth_client_id"
/// How long browser login waits for the user, in seconds; 0 waits until the login
/// code expires. Defaults to 120.
#define ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT "motherduck.login_timeout"

namespace adbcpq {

/// \brief The URI in a form libpq parses. The driver accepts only motherduck://,
/// md:// and md: URIs: [user[:token]@][host][:port]/[database][?params] after
/// the first two, [database][?params] after md:, where an empty database is the
/// workspace. ADBC driver managers pass motherduck:// through unchanged when they
/// pick this driver from the scheme. This rewrites the URI as a postgresql://
/// URI, which libpq parses and which stays an implementation detail of the driver.
/// Anything else is returned as is. The query is left in place; the driver strips
/// and interprets it itself.
std::string NormalizeUri(std::string_view uri);

/// \brief The driver's settings that a database option or a URI query parameter
/// of the same name can set. A string is empty when unset.
struct MotherduckSettings {
  std::string attach_mode;
  std::string session_name;
  std::string application_name;
  std::string connect_timeout;
  std::string region;
  std::string db_schema;
  std::string tls_root_cert_path;
  std::optional<bool> tls_verify;
  std::string auth_type;
  std::string oauth_client_id;
  std::string login_timeout;
};

/// \brief The Postgres endpoint host for a MotherDuck region, e.g.
/// pg.us-east-1-aws.motherduck.com for us-east-1 or
/// pg.eastus2-azure.motherduck.com for eastus2, or empty if region can't be
/// part of a hostname.
std::string RegionHost(std::string_view region);

/// \brief Whether the driver chooses sslmode (verify-full): PGSSLMODE doesn't set
/// sslmode, and sslrootcert (from PGSSLROOTCERT) isn't system, which makes libpq
/// default to verify-full.
bool DriverChoosesSslmode();

/// \brief message with every occurrence of the password, and of the user unless it's
/// the default one, replaced by <redacted>. libpq's connection errors quote the
/// user, which is a token if it was put where the user goes.
std::string ScrubConnectError(std::string_view message, std::string_view user,
                              std::string_view password);

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

/// \brief A libpq session option, --<name>=<value>, with whitespace and
/// backslashes in value escaped.
std::string SessionOption(std::string_view name, std::string_view value);

class MotherduckDatabase : public PostgresDatabase {
 public:
  // The endpoint rejects binary COPY TO STDOUT, so results don't use it unless
  // adbc.postgresql.use_copy asks for it (#11).
  MotherduckDatabase() { use_copy_ = false; }

  AdbcStatusCode Init(struct AdbcError* error) override;
  AdbcStatusCode GetOption(const char* option, char* value, size_t* length,
                           struct AdbcError* error) override;
  AdbcStatusCode GetOptionInt(const char* option, int64_t* value,
                              struct AdbcError* error) override;
  AdbcStatusCode SetOption(const char* key, const char* value,
                           struct AdbcError* error) override;
  AdbcStatusCode SetOptionInt(const char* key, int64_t value,
                              struct AdbcError* error) override;
  AdbcStatusCode Connect(PGconn** conn, struct AdbcError* error) override;

  /// \brief The libpq keywords and values Connect passes, in order. With browser
  ///   login, these don't include the token, and they include the host only if
  ///   the URI, an environment variable or the region names one.
  AdbcStatusCode ConnectParams(std::vector<std::pair<std::string, std::string>>* params,
                               struct AdbcError* error) const;

  /// \brief Replace how Connect finds a CA bundle when the connection names
  /// none, for tests. Returns the bundle's path, or empty if there's none.
  void SetCaBundleFinder(std::function<std::string()> finder) {
    find_ca_bundle_ = std::move(finder);
  }

  std::string_view InfoVendorName() override { return "MotherDuck"; }
  std::string_view DriverName() override { return "ADBC MotherDuck Driver"; }
  std::string_view DriverVersion() override;

 private:
  // The settings the URI's query parameters set, and the ones database options
  // set, which take precedence.
  MotherduckSettings uri_settings_;
  MotherduckSettings option_settings_;
  // The settings in effect: an option's value if it has one, else the URI's.
  MotherduckSettings Settings() const;
  std::string username_;
  std::string password_;
  // Empty for FindCaBundle.
  std::function<std::string()> find_ca_bundle_;
  // Set by Init with browser login. Connect logs in exactly when it's set, so
  // changing motherduck.auth_type after Init has no effect.
  std::unique_ptr<auth::TokenCache> token_cache_;

  AdbcStatusCode ConnectParams(std::vector<std::pair<std::string, std::string>>* params,
                               bool browser_login, struct AdbcError* error) const;
};

}  // namespace adbcpq
