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

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "motherduck_database.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "adbc_driver_motherduck_config.h"
#include "driver/common/utils.h"
#include "motherduck_type_resolver.h"

namespace adbcpq {

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
}  // namespace

std::string AddAttachMode(std::string_view options, std::string_view attach_mode) {
  std::string result(options);
  if (SetsAttachMode(options)) return result;
  if (!result.empty()) result += ' ';
  result += "--attach_mode=";
  result += attach_mode;
  return result;
}

std::string_view MotherduckDatabase::DriverVersion() {
  return ADBC_DRIVER_MOTHERDUCK_VERSION;
}

AdbcStatusCode MotherduckDatabase::Init(struct AdbcError* error) {
  // Fail fast on a bad URI or token. This replaces PostgresDatabase::Init
  // rather than calling it: the server version and pg_type it reads are
  // PostgreSQL's, and mustn't reach MotherDuck users.
  PGconn* conn = nullptr;
  RAISE_ADBC(Connect(&conn, error));
  RAISE_ADBC(Disconnect(&conn, error));

  Status status = SharedMotherduckTypeResolver(&type_resolver_);
  return status.ToAdbc(error);
}

AdbcStatusCode MotherduckDatabase::GetOption(const char* option, char* value,
                                             size_t* length, struct AdbcError* error) {
  std::string output;
  if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = attach_mode_;
  } else if (std::strcmp(option, "adbc.postgresql.type_resolver_mode") == 0) {
    // A PostgreSQL option, accepted by SetOption only for compatibility
    InternalAdbcSetError(error, "[libpq] unknown database option '%s'", option);
    return ADBC_STATUS_NOT_FOUND;
  } else {
    return PostgresDatabase::GetOption(option, value, length, error);
  }
  if (output.size() + 1 <= *length) {
    std::memcpy(value, output.c_str(), output.size() + 1);
  }
  *length = output.size() + 1;
  return ADBC_STATUS_OK;
}

AdbcStatusCode MotherduckDatabase::SetOption(const char* key, const char* value,
                                             struct AdbcError* error) {
  if (value == nullptr) {
    InternalAdbcSetError(error, "[libpq] Option %s requires a value", key);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (std::strcmp(key, "adbc.postgresql.type_resolver_mode") == 0) {
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
  } else {
    return PostgresDatabase::SetOption(key, value, error);
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode MotherduckDatabase::Connect(PGconn** conn, struct AdbcError* error) {
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

}  // namespace adbcpq
