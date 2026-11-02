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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.h>

#include "driver/common/utils.h"
#include "postgres_util.h"

namespace adbcpq {

namespace {
// Whether libpq session options set a setting, as --<name>=<value> or
// -c <name>=<value>, with either underscores or dashes in the name.
bool SetsSessionOption(std::string_view options, std::string_view name) {
  std::string underscores(name);
  std::string dashes(name);
  for (char& c : dashes) {
    if (c == '_') c = '-';
  }
  underscores += '=';
  dashes += '=';
  // Split on whitespace, as libpq does, keeping backslash escapes in each arg.
  bool after_c = false;
  size_t i = 0;
  while (i < options.size()) {
    if (std::isspace(static_cast<unsigned char>(options[i]))) {
      i++;
      continue;
    }
    size_t start = i;
    while (i < options.size() && !std::isspace(static_cast<unsigned char>(options[i]))) {
      if (options[i] == '\\' && i + 1 < options.size()) i++;
      i++;
    }
    std::string_view arg = options.substr(start, i - start);
    // A bare <name>=<value> only counts as a setting right after -c.
    bool setting_arg = after_c;
    after_c = arg == "-c";
    if (arg.substr(0, 2) == "--" || arg.substr(0, 2) == "-c") {
      arg.remove_prefix(2);
      setting_arg = true;
    }
    if (!setting_arg) continue;
    for (std::string_view setting : {underscores, dashes}) {
      if (arg.substr(0, setting.size()) == setting) return true;
    }
  }
  return false;
}

// ASCII-only, unlike std::islower and friends, which follow the C locale.
bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Whether value is a region: two or more dash-separated parts of lowercase
// letters and digits, with a digit somewhere, e.g. us-east-1.
bool IsRegion(std::string_view value) {
  bool part_empty = true;
  bool has_digit = false;
  int parts = 1;
  for (char c : value) {
    if (c == '-') {
      if (part_empty) return false;
      part_empty = true;
      parts++;
    } else if (IsLower(c) || IsDigit(c)) {
      part_empty = false;
      has_digit = has_digit || IsDigit(c);
    } else {
      return false;
    }
  }
  return !part_empty && parts >= 2 && has_digit;
}

// Whether value can go into libpq options as is: libpq splits options on
// whitespace, and MotherDuck isn't documented to honor libpq's backslash
// escapes.
bool IsOptionValue(std::string_view value) {
  for (char c : value) {
    if (c == '\\' || std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}
}  // namespace

std::string RegionHost(std::string_view region) {
  std::string host = "pg.";
  host += region;
  // A last part of only letters names the cloud (us-east-1-aws); otherwise
  // it's AWS.
  std::string_view last = region.substr(region.rfind('-') + 1);
  if (last.empty() || !std::all_of(last.begin(), last.end(), IsLower)) host += "-aws";
  host += ".motherduck.com";
  return host;
}

std::string AddSessionOption(std::string_view options, std::string_view name,
                             std::string_view value) {
  std::string result(options);
  if (SetsSessionOption(options, name)) return result;
  if (!result.empty()) result += ' ';
  result += "--";
  result += name;
  result += '=';
  result += value;
  return result;
}

PostgresDatabase::PostgresDatabase() : open_connections_(0) {
  type_resolver_ = std::make_shared<PostgresTypeResolver>();
}
PostgresDatabase::~PostgresDatabase() = default;

AdbcStatusCode PostgresDatabase::GetOption(const char* option, char* value,
                                           size_t* length, struct AdbcError* error) {
  std::string output;
  // Options that are unset when empty.
  const std::string* optional = nullptr;
  if (std::strcmp(option, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    output = use_copy_ ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = attach_mode_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    optional = &session_name_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    optional = &region_;
  } else if (std::strcmp(option, ADBC_OPTION_USERNAME) == 0) {
    optional = &username_;
  } else if (std::strcmp(option, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(option, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0) {
    InternalAdbcSetError(error, "[libpq] database option '%s' can't be read", option);
    return ADBC_STATUS_NOT_FOUND;
  } else {
    InternalAdbcSetError(error, "[libpq] unknown database option '%s'", option);
    return ADBC_STATUS_NOT_FOUND;
  }
  if (optional != nullptr) {
    if (optional->empty()) {
      InternalAdbcSetError(error, "[libpq] database option '%s' is not set", option);
      return ADBC_STATUS_NOT_FOUND;
    }
    output = *optional;
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
  return ADBC_STATUS_NOT_FOUND;
}
AdbcStatusCode PostgresDatabase::GetOptionDouble(const char* option, double* value,
                                                 struct AdbcError* error) {
  return ADBC_STATUS_NOT_FOUND;
}

AdbcStatusCode PostgresDatabase::Init(struct AdbcError* error) {
  // Fail fast on a bad URI or token
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
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    if (!IsOptionValue(value)) {
      InternalAdbcSetError(error,
                           "[libpq] Invalid value for option %s=%s: whitespace and "
                           "backslashes aren't allowed",
                           key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    session_name_ = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    // The region becomes part of a hostname. An empty value unsets it.
    if (*value != '\0' && !IsRegion(value)) {
      InternalAdbcSetError(error,
                           "[libpq] Invalid value for option %s=%s: expected a region "
                           "such as us-east-1",
                           key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    region_ = value;
  } else if (std::strcmp(key, ADBC_OPTION_USERNAME) == 0) {
    username_ = value;
  } else if (std::strcmp(key, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(key, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0) {
    password_ = value;
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
  InternalAdbcSetError(error, "%s%s", "[libpq] Unknown option ", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode PostgresDatabase::ConnectParams(
    std::vector<std::pair<std::string, std::string>>* params,
    struct AdbcError* error) const {
  if (uri_.empty()) {
    InternalAdbcSetError(
        error, "%s",
        "[libpq] Must set database option 'uri' before creating a connection");
    return ADBC_STATUS_INVALID_STATE;
  }

  char* parse_error = nullptr;
  PQconninfoOption* conninfo = PQconninfoParse(uri_.c_str(), &parse_error);
  if (conninfo == nullptr) {
    InternalAdbcSetError(error, "%s%s", "[libpq] Failed to connect: ",
                         parse_error ? parse_error : "out of memory");
    PQfreemem(parse_error);
    return ADBC_STATUS_IO;
  }

  // Merge the MotherDuck settings into the session options from the URI (or
  // PGOPTIONS), apply the region option, and fill in the user and token. The driver
  // always passes both, so libpq never falls back to PGUSER, PGPASSWORD, or a password
  // file.
  params->clear();
  const char* env_options = std::getenv("PGOPTIONS");
  std::string options = env_options ? env_options : "";
  std::string password;
  std::string user;
  bool uri_sets_host = false;
  bool uri_sets_sslmode = false;
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    std::string_view keyword = option->keyword;
    if ((keyword == "host" || keyword == "hostaddr") && *option->val != '\0') {
      uri_sets_host = true;
    }
    if (keyword == "sslmode" && *option->val != '\0') uri_sets_sslmode = true;
    if (keyword == "options") {
      options = option->val;
    } else if (keyword == "password") {
      password = option->val;
    } else if (keyword == "user") {
      user = option->val;
    } else {
      params->emplace_back(option->keyword, option->val);
    }
  }
  PQconninfoFree(conninfo);

  if (!region_.empty()) {
    // libpq would still apply PGHOSTADDR, connecting to that address
    // instead of the region's host.
    const char* env_hostaddr = std::getenv("PGHOSTADDR");
    if (uri_sets_host || (env_hostaddr != nullptr && *env_hostaddr != '\0')) {
      InternalAdbcSetError(error,
                           "[libpq] Set either database option '%s' or a host "
                           "(host or hostaddr in the URI, or PGHOSTADDR), not both",
                           ADBC_MOTHERDUCK_OPTION_REGION);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    params->emplace_back("host", RegionHost(region_));
    // The driver chose the host, so don't let libpq's default sslmode=prefer
    // fall back to sending the token in plaintext.
    const char* env_sslmode = std::getenv("PGSSLMODE");
    if (!uri_sets_sslmode && (env_sslmode == nullptr || *env_sslmode == '\0')) {
      params->emplace_back("sslmode", "require");
    }
  }
  if (!password_.empty()) password = password_;
  if (password.empty()) {
    const char* token = std::getenv("MOTHERDUCK_TOKEN");
    if (token != nullptr) password = token;
  }
  if (password.empty()) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] No MotherDuck token: set the 'password' (or "
                         "'adbc.motherduck.token') database option, a password in "
                         "the URI, or MOTHERDUCK_TOKEN");
    return ADBC_STATUS_UNAUTHENTICATED;
  }
  params->emplace_back("password", std::move(password));
  // MotherDuck's endpoint expects the user postgres.
  if (!username_.empty()) user = username_;
  if (user.empty()) user = "postgres";
  params->emplace_back("user", std::move(user));
  options = AddSessionOption(options, "attach_mode", attach_mode_);
  if (!session_name_.empty()) {
    options = AddSessionOption(options, "session_name", session_name_);
  }
  params->emplace_back("options", std::move(options));
  return ADBC_STATUS_OK;
}

AdbcStatusCode PostgresDatabase::Connect(PGconn** conn, struct AdbcError* error) {
  std::vector<std::pair<std::string, std::string>> params;
  RAISE_ADBC(ConnectParams(&params, error));
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
    InternalAdbcSetError(error, "%s%s",
                         "[libpq] Failed to connect: ", PQerrorMessage(*conn));
    PQfinish(*conn);
    *conn = nullptr;
    return ADBC_STATUS_IO;
  }
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
