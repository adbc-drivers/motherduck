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
    if (arg.substr(0, 2) == "--" || arg.substr(0, 2) == "-c") arg.remove_prefix(2);
    for (std::string_view setting : {underscores, dashes}) {
      if (arg.substr(0, setting.size()) == setting) return true;
    }
  }
  return false;
}
}  // namespace

std::string AddSessionOption(std::string_view options, std::string_view name,
                             std::string_view value) {
  std::string result(options);
  if (SetsSessionOption(options, name)) return result;
  if (!result.empty()) result += ' ';
  result += "--";
  result += name;
  result += '=';
  // libpq splits options on whitespace unless it's escaped with a backslash.
  for (char c : value) {
    if (c == '\\' || std::isspace(static_cast<unsigned char>(c))) result += '\\';
    result += c;
  }
  return result;
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
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0 &&
             !session_name_.empty()) {
    output = session_name_;
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
    session_name_ = value;
  } else if (std::strcmp(key, ADBC_OPTION_PASSWORD) == 0) {
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

AdbcStatusCode PostgresDatabase::Connect(PGconn** conn, struct AdbcError* error) {
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
  // PGOPTIONS), and fill in the password.
  std::vector<const char*> keywords;
  std::vector<const char*> values;
  const char* options = std::getenv("PGOPTIONS");
  const char* password = std::getenv("PGPASSWORD");
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    if (std::strcmp(option->keyword, "options") == 0) {
      options = option->val;
      continue;
    }
    if (std::strcmp(option->keyword, "password") == 0) {
      password = option->val;
      continue;
    }
    keywords.push_back(option->keyword);
    values.push_back(option->val);
  }
  std::string session_options =
      AddSessionOption(options ? options : "", "attach_mode", attach_mode_);
  if (!session_name_.empty()) {
    session_options = AddSessionOption(session_options, "session_name", session_name_);
  }
  keywords.push_back("options");
  values.push_back(session_options.c_str());
  if (!password_.empty()) password = password_.c_str();
  if (password == nullptr) password = std::getenv("MOTHERDUCK_TOKEN");
  if (password != nullptr) {
    keywords.push_back("password");
    values.push_back(password);
  }
  keywords.push_back(nullptr);
  values.push_back(nullptr);

  *conn = PQconnectdbParams(keywords.data(), values.data(), /*expand_dbname=*/0);
  PQconninfoFree(conninfo);
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
