// Copyright (c) 2026 ADBC Drivers Contributors
//
// This file has been modified from its original version, which is
// under the Apache License:
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "database.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.h>

#include "auth/login.h"
#include "driver/common/utils.h"
#include "postgres_util.h"

namespace adbcpq {

using adbc::driver::Result;

namespace {
// Whether libpq session options set attach_mode, as --attach_mode=<mode> or
// -c attach_mode=<mode> (or attach-mode).
bool SetsAttachMode(std::string_view options) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  constexpr std::string_view kSettings[] = {"attach_mode=", "attach-mode="};
  size_t start = options.find_first_not_of(kWhitespace);
  while (start != std::string_view::npos) {
    size_t end = options.find_first_of(kWhitespace, start);
    std::string_view arg = options.substr(start, end - start);
    if (arg.substr(0, 2) == "--" || arg.substr(0, 2) == "-c") arg.remove_prefix(2);
    for (std::string_view setting : kSettings) {
      if (arg.substr(0, setting.size()) == setting) return true;
    }
    start = options.find_first_not_of(kWhitespace, end);
  }
  return false;
}

constexpr std::string_view kAuthTypeToken = "token";
constexpr std::string_view kAuthTypeBrowser = "browser";

bool HasParam(const ConnectParams& params, std::string_view keyword) {
  for (const auto& [name, value] : params) {
    if (name == keyword && !value.empty()) return true;
  }
  return false;
}

bool HasEnv(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
}

// The URI's parameters, with attach_mode merged into its session options (or
// PGOPTIONS).
Status ParseUri(const std::string& uri, std::string_view attach_mode,
                ConnectParams* params) {
  char* parse_error = nullptr;
  PQconninfoOption* conninfo = PQconninfoParse(uri.c_str(), &parse_error);
  if (conninfo == nullptr) {
    Status status(ADBC_STATUS_IO, std::string("[libpq] Failed to connect: ") +
                                      (parse_error ? parse_error : "out of memory"));
    PQfreemem(parse_error);
    return status;
  }
  const char* options = std::getenv("PGOPTIONS");
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    if (std::strcmp(option->keyword, "options") == 0) {
      options = option->val;
      continue;
    }
    params->emplace_back(option->keyword, option->val);
  }
  params->emplace_back("options", AddAttachMode(options ? options : "", attach_mode));
  PQconninfoFree(conninfo);
  return Status::Ok();
}

Status OpenConnection(const ConnectParams& params, PGconn** conn) {
  std::vector<const char*> keywords;
  std::vector<const char*> values;
  for (const auto& [keyword, value] : params) {
    keywords.push_back(keyword.c_str());
    values.push_back(value.c_str());
  }
  keywords.push_back(nullptr);
  values.push_back(nullptr);
  *conn = PQconnectdbParams(keywords.data(), values.data(), /*expand_dbname=*/0);
  if (PQstatus(*conn) != CONNECTION_OK) {
    Status status(ADBC_STATUS_IO,
                  std::string("[libpq] Failed to connect: ") + PQerrorMessage(*conn));
    PQfinish(*conn);
    *conn = nullptr;
    return status;
  }
  return Status::Ok();
}

// The parameters with browser login's token as the password and, if the URI names
// no host, the organization's Postgres endpoint as the host.
Result<ConnectParams> WithBrowserLogin(auth::TokenCache& token_cache,
                                       const ConnectParams& params,
                                       auth::Credentials* credentials) {
  if (HasParam(params, "password")) {
    return Status(ADBC_STATUS_INVALID_ARGUMENT,
                  "[motherduck] The URI must not contain a password when "
                  "adbc.motherduck.auth_type is browser");
  }
  // Without a host, libpq would try a local socket. MotherDuck's endpoints are
  // regional, so use the one of the user's organization.
  const bool need_host = !HasParam(params, "host") && !HasParam(params, "hostaddr") &&
                         !HasEnv("PGHOST") && !HasEnv("PGHOSTADDR");
  UNWRAP_STATUS(token_cache.Get(need_host, credentials));
  return AddCredentials(params, *credentials, need_host);
}

// libpq's root certificate, which browser login also trusts for its HTTPS requests.
std::string CaBundle(const std::string& uri) {
  std::string bundle;
  ConnectParams params;
  if (ParseUri(uri, "", &params).ok()) {
    for (const auto& [keyword, value] : params) {
      if (keyword == "sslrootcert") bundle = value;
    }
  }
  if (bundle.empty() && HasEnv("PGSSLROOTCERT")) bundle = std::getenv("PGSSLROOTCERT");
  // libpq's "system" means the system's trust store, the login's default too.
  return bundle == "system" ? "" : bundle;
}

bool ParseLoginTimeout(std::string_view text, int64_t* seconds) {
  int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size() || value < 0) return false;
  *seconds = value;
  return true;
}
}  // namespace

std::string AddAttachMode(std::string_view options, std::string_view attach_mode) {
  std::string result(options);
  if (SetsAttachMode(options)) return result;
  if (!result.empty()) result += ' ';
  result += "--attach_mode=";
  result += attach_mode;
  return result;
}

bool IsAuthFailure(std::string_view message) {
  // The first answers a password that isn't a MotherDuck token at all; the others
  // come from validating one.
  constexpr std::string_view kMessages[] = {"invalid username/password",
                                            "Invalid token or user", "Jwt is expired",
                                            "user not found", "not authenticated"};
  for (std::string_view known : kMessages) {
    if (message.find(known) != std::string_view::npos) return true;
  }
  return false;
}

ConnectParams AddCredentials(ConnectParams params, const auth::Credentials& credentials,
                             bool need_host) {
  params.emplace_back("password", credentials.token);
  if (need_host) params.emplace_back("host", credentials.host);
  return params;
}

PostgresDatabase::PostgresDatabase() : open_connections_(0) {
  type_resolver_ = std::make_shared<PostgresTypeResolver>();
}
PostgresDatabase::~PostgresDatabase() = default;

AdbcStatusCode PostgresDatabase::GetOption(const char* option, char* value,
                                           size_t* length, struct AdbcError* error) {
  std::string output;
  if (std::strcmp(option, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    output = use_copy_ ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = attach_mode_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_AUTH_TYPE) == 0) {
    output = auth_type_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_OAUTH_CLIENT_ID) == 0) {
    output = oauth_client_id_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT) == 0) {
    output = std::to_string(login_timeout_seconds_);
  } else {
    InternalAdbcSetError(error, "[libpq] unknown database option '%s'", option);
    return ADBC_STATUS_NOT_FOUND;
  }
  if (output.size() + 1 <= *length) {
    std::memcpy(value, output.c_str(), output.size() + 1);
  }
  *length = output.size() + 1;
  return ADBC_STATUS_OK;
}
AdbcStatusCode PostgresDatabase::GetOptionBytes(const char* option, uint8_t* value,
                                                size_t* length, struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}
AdbcStatusCode PostgresDatabase::GetOptionInt(const char* option, int64_t* value,
                                              struct AdbcError* error) {
  if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT) == 0) {
    *value = login_timeout_seconds_;
    return ADBC_STATUS_OK;
  }
  return ADBC_STATUS_NOT_FOUND;
}
AdbcStatusCode PostgresDatabase::GetOptionDouble(const char* option, double* value,
                                                 struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresDatabase::Init(struct AdbcError* error) {
  if (auth_type_ == kAuthTypeBrowser) {
    auth::LoginOptions options;
    options.client_id = oauth_client_id_;
    options.api_origin = auth::ApiOrigin();
    options.timeout = std::chrono::seconds(login_timeout_seconds_);
    token_cache_ = std::make_unique<auth::TokenCache>(
        auth::DefaultLoginEnvironment(CaBundle(uri_)), std::move(options));
  }

  // Fail fast on a bad URI or token; with browser login, this logs in.
  PGconn* conn = nullptr;
  RAISE_ADBC(Connect(&conn, error));
  RAISE_ADBC(Disconnect(&conn, error));

  Status status = SharedTypeResolver(&type_resolver_);
  return status.ToAdbc(error);
}

AdbcStatusCode PostgresDatabase::Release(struct AdbcError* error) {
  if (open_connections_ != 0) {
    InternalAdbcSetError(error, "%s%" PRId32 "%s", "[libpq] Database released with ",
                         open_connections_, " open connections");
    return ADBC_STATUS_INVALID_STATE;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::SetOption(const char* key, const char* value,
                                           struct AdbcError* error) {
  if (std::strcmp(key, "uri") == 0) {
    uri_ = value;
  } else if (std::strcmp(key, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    if (std::strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      use_copy_ = true;
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      use_copy_ = false;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else if (std::strcmp(key, "adbc.postgresql.type_resolver_mode") == 0) {
    // Accepted for compatibility with the PostgreSQL driver, but ignored. Warn once
    // per process so connection pools don't repeat it.
    // TODO(https://github.com/adbc-drivers/motherduck/issues/29): use a logger
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      std::fprintf(stderr,
                   "[motherduck] warning: ignoring database option %s=%s; the driver "
                   "always uses its built-in type table\n",
                   key, value);
    }
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    if (std::strcmp(value, "single") != 0 && std::strcmp(value, "workspace") != 0) {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    attach_mode_ = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_AUTH_TYPE) == 0) {
    if (value == nullptr || (value != kAuthTypeToken && value != kAuthTypeBrowser)) {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key,
                           value ? value : "(null)");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    auth_type_ = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_OAUTH_CLIENT_ID) == 0) {
    if (value == nullptr || *value == '\0') {
      InternalAdbcSetError(error, "[libpq] Option %s must not be empty", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    oauth_client_id_ = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT) == 0) {
    if (value == nullptr || !ParseLoginTimeout(value, &login_timeout_seconds_)) {
      InternalAdbcSetError(error,
                           "[libpq] Invalid value for option %s=%s; expected a "
                           "non-negative number of seconds",
                           key, value ? value : "(null)");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else {
    InternalAdbcSetError(error, "%s%s", "[libpq] Unknown database option ", key);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::SetOptionBytes(const char* key, const uint8_t* value,
                                                size_t length, struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::SetOptionDouble(const char* key, double value,
                                                 struct AdbcError* error) {
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::SetOptionInt(const char* key, int64_t value,
                                              struct AdbcError* error) {
  if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT) == 0) {
    if (value < 0) {
      InternalAdbcSetError(error,
                           "[libpq] Invalid value for option %s=%" PRId64
                           "; expected a non-negative number of seconds",
                           key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    login_timeout_seconds_ = value;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::Connect(PGconn** conn, struct AdbcError* error) {
  if (uri_.empty()) {
    InternalAdbcSetError(
        error, "%s",
        "[libpq] Must set database option 'uri' before creating a connection");
    return ADBC_STATUS_INVALID_STATE;
  }

  ConnectParams params;
  RAISE_STATUS(error, ParseUri(uri_, attach_mode_, &params));
  ConnectParams connect_params = params;
  auth::Credentials credentials;
  if (token_cache_) {
    RAISE_RESULT(error, connect_params,
                 WithBrowserLogin(*token_cache_, params, &credentials));
  }
  Status status = OpenConnection(connect_params, conn);
  if (!status.ok() && token_cache_ && !credentials.fresh &&
      IsAuthFailure(status.message()) && token_cache_->Reject(credentials.token)) {
    // The saved token expired or was revoked: log in again, once.
    RAISE_RESULT(error, connect_params,
                 WithBrowserLogin(*token_cache_, params, &credentials));
    status = OpenConnection(connect_params, conn);
  }
  RAISE_STATUS(error, status);
  open_connections_++;
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::Disconnect(PGconn** conn, struct AdbcError* error) {
  PQfinish(*conn);
  *conn = nullptr;
  if (--open_connections_ < 0) {
    InternalAdbcSetError(error, "%s", "[libpq] Open connection count underflowed");
    return ADBC_STATUS_INTERNAL;
  }
  return ADBC_STATUS_OK;
}

}  // namespace adbcpq
