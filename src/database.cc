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

#include <cinttypes>
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
// Whether libpq session options set attach_mode, as --attach_mode=<mode> or
// -c attach_mode=<mode>.
bool SetsAttachMode(std::string_view options) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  constexpr std::string_view kSetting = "attach_mode=";
  size_t start = options.find_first_not_of(kWhitespace);
  while (start != std::string_view::npos) {
    size_t end = options.find_first_of(kWhitespace, start);
    std::string_view arg = options.substr(start, end - start);
    if (arg.substr(0, 2) == "--" || arg.substr(0, 2) == "-c") arg.remove_prefix(2);
    if (arg.substr(0, kSetting.size()) == kSetting) return true;
    start = options.find_first_not_of(kWhitespace, end);
  }
  return false;
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

PostgresDatabase::PostgresDatabase() : open_connections_(0) {
  type_resolver_ = std::make_shared<PostgresTypeResolver>();
}
PostgresDatabase::~PostgresDatabase() = default;

AdbcStatusCode PostgresDatabase::GetOption(const char* option, char* value,
                                           size_t* length, struct AdbcError* error) {
  std::string output;
  if (std::strcmp(option, ADBC_POSTGRESQL_OPTION_USE_COPY) == 0) {
    output = use_copy_ ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (std::strcmp(option, "adbc.postgresql.type_resolver_mode") == 0) {
    switch (type_resolver_mode_) {
      case TypeResolverMode::kBuiltin:
        output = "builtin";
        break;
      case TypeResolverMode::kServer:
        output = "server";
        break;
      default:
        InternalAdbcSetError(error, "[libpq] unknown type resolver mode");
        return ADBC_STATUS_INTERNAL;
    }
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = attach_mode_;
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
  // Connect to build the type table
  PGconn* conn = nullptr;
  RAISE_ADBC(Connect(&conn, error));

  auto resolver = std::make_shared<PostgresTypeResolver>();
  Status status;
  switch (type_resolver_mode_) {
    case TypeResolverMode::kBuiltin: {
      status = InitializeTypeResolver(*resolver);
      break;
    }
    case TypeResolverMode::kServer: {
      status = RebuildTypeResolver(conn, *resolver);
      break;
    }
  }
  if (!status.ok()) {
    RAISE_ADBC(Disconnect(&conn, nullptr));
    return status.ToAdbc(error);
  }

  type_resolver_ = std::move(resolver);

  RAISE_ADBC(Disconnect(&conn, nullptr));
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
    if (std::strcmp(value, "auto") == 0) {
      // TODO(https://github.com/apache/arrow-adbc/issues/4782): implement fallback mode
      InternalAdbcSetError(error, "[libpq] %s=%s not yet supported", key, value);
      return ADBC_STATUS_NOT_IMPLEMENTED;
    } else if (std::strcmp(value, "builtin") == 0) {
      type_resolver_mode_ = TypeResolverMode::kBuiltin;
    } else if (std::strcmp(value, "server") == 0) {
      type_resolver_mode_ = TypeResolverMode::kServer;
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    if (std::strcmp(value, "single") != 0 && std::strcmp(value, "workspace") != 0) {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s", key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    attach_mode_ = value;
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

  // Merge attach_mode into the session options from the URI (or PGOPTIONS).
  std::vector<const char*> keywords;
  std::vector<const char*> values;
  const char* options = std::getenv("PGOPTIONS");
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    if (std::strcmp(option->keyword, "options") == 0) {
      options = option->val;
      continue;
    }
    keywords.push_back(option->keyword);
    values.push_back(option->val);
  }
  std::string session_options = AddAttachMode(options ? options : "", attach_mode_);
  keywords.push_back("options");
  values.push_back(session_options.c_str());
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
