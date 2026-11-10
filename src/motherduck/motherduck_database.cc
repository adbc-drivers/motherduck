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
// winsock2.h includes windows.h, which declares GetEnvironmentVariableA.
#include <winsock2.h>
#endif

#include "motherduck_database.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "adbc_driver_motherduck_config.h"
#include "driver/common/utils.h"
#include "motherduck_ascii.h"
#include "motherduck_type_resolver.h"

namespace adbcpq {

namespace {
constexpr char kDefaultAttachMode[] = "single";
// MotherDuck's endpoint expects the user postgres.
constexpr char kDefaultUser[] = "postgres";

// An environment variable, or empty if it's unset. On Windows this reads the
// process environment rather than this DLL's own (static) C runtime copy, which
// misses changes the application makes after the DLL loads.
std::string GetEnv(const char* name) {
#ifdef _WIN32
  std::string value;
  DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
  while (size > 0) {
    value.resize(size);
    DWORD written = GetEnvironmentVariableA(name, value.data(), size);
    // Less than the buffer: the value and its terminator fit. More: it grew
    // meanwhile, so retry with the new size.
    if (written < size) {
      value.resize(written);
      return value;
    }
    size = written;
  }
  return "";
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

// A host for comparison: lowercase, without a trailing dot.
std::string NormalizeHost(std::string_view host) {
  if (!host.empty() && host.back() == '.') host.remove_suffix(1);
  return AsciiLower(host);
}

// Whether options ends in a backslash that escapes nothing, which would escape
// the space before any session option appended after it.
bool EndsWithLoneBackslash(std::string_view options) {
  size_t backslashes = 0;
  while (backslashes < options.size() &&
         options[options.size() - 1 - backslashes] == '\\') {
    backslashes++;
  }
  return backslashes % 2 == 1;
}

// A keyword's value in a libpq URI or connection string (empty if unset), or
// false if the URI doesn't parse.
bool UriValue(const std::string& uri, std::string_view keyword, std::string* value) {
  PQconninfoOption* conninfo = PQconninfoParse(uri.c_str(), nullptr);
  if (conninfo == nullptr) return false;
  value->clear();
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val != nullptr && keyword == option->keyword) *value = option->val;
  }
  PQconninfoFree(conninfo);
  return true;
}

// Whether value can be part of a hostname: dash-separated parts of lowercase
// ASCII letters and digits. This keeps out anything libpq treats specially in
// a host, such as a comma, which separates hosts it would also try.
bool IsHostLabel(std::string_view value) {
  // DNS limits a label to 63 characters; with the cloud suffix this leaves room
  // for any real region.
  if (value.size() > 63 - std::string_view("-azure").size()) return false;
  bool part_empty = true;
  for (char c : value) {
    if (c == '-') {
      if (part_empty) return false;
      part_empty = true;
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      part_empty = false;
    } else {
      return false;
    }
  }
  return !part_empty;
}

// Backslash-escapes whitespace and backslashes for libpq options. MotherDuck
// removes the escapes, so "a b" arrives as is.
std::string EscapeOptionValue(std::string_view value) {
  std::string escaped;
  for (char c : value) {
    if (c == '\\' || std::isspace(static_cast<unsigned char>(c))) escaped += '\\';
    escaped += c;
  }
  return escaped;
}
}  // namespace

std::string RegionHost(std::string_view region) {
  if (!IsHostLabel(region)) return "";
  std::string host = "pg.";
  host += region;
  // Without a cloud suffix, AWS names have dashes (us-east-1) and Azure names
  // don't (eastus2).
  if (!region.ends_with("-aws") && !region.ends_with("-azure")) {
    host += region.find('-') == std::string_view::npos ? "-azure" : "-aws";
  }
  host += ".motherduck.com";
  return host;
}

bool DefaultsToRequireSsl(std::string_view uri_sslmode,
                          std::string_view uri_sslrootcert) {
  if (!uri_sslmode.empty() || !GetEnv("PGSSLMODE").empty()) return false;
  // With sslrootcert=system, libpq already defaults to verify-full and rejects
  // anything weaker.
  std::string sslrootcert =
      uri_sslrootcert.empty() ? GetEnv("PGSSLROOTCERT") : std::string(uri_sslrootcert);
  return sslrootcert != "system";
}

std::string SessionOption(std::string_view name, std::string_view value) {
  std::string option = "--";
  option += name;
  option += '=';
  option += EscapeOptionValue(value);
  return option;
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
  // Options that are unset when empty.
  const std::string* optional = nullptr;
  if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = attach_mode_.empty() ? kDefaultAttachMode : attach_mode_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    optional = &session_name_;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    optional = &region_;
  } else if (std::strcmp(option, ADBC_OPTION_USERNAME) == 0) {
    // The user the driver connects as.
    output = username_;
    if (output.empty() && !uri_.empty() && !UriValue(uri_, "user", &output)) {
      InternalAdbcSetError(error,
                           "[libpq] database option '%s' is unknown: the URI doesn't "
                           "parse",
                           option);
      return ADBC_STATUS_INVALID_STATE;
    }
    if (output.empty()) output = kDefaultUser;
  } else if (std::strcmp(option, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(option, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0) {
    InternalAdbcSetError(error, "[libpq] database option '%s' can't be read", option);
    return ADBC_STATUS_NOT_FOUND;
  } else if (std::strcmp(option, "adbc.postgresql.type_resolver_mode") == 0) {
    // A PostgreSQL option, accepted by SetOption only for compatibility
    InternalAdbcSetError(error, "[libpq] unknown database option '%s'", option);
    return ADBC_STATUS_NOT_FOUND;
  } else {
    return PostgresDatabase::GetOption(option, value, length, error);
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
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    session_name_ = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    // The region becomes part of a hostname. An empty value unsets it.
    std::string region = AsciiLower(value);
    if (!region.empty() && RegionHost(region).empty()) {
      InternalAdbcSetError(error,
                           "[libpq] Invalid value for option %s=%s: expected a region "
                           "such as us-east-1 or eastus2, of letters, digits and "
                           "dashes",
                           key, value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    region_ = std::move(region);
  } else if (std::strcmp(key, ADBC_OPTION_USERNAME) == 0) {
    username_ = value;
  } else if (std::strcmp(key, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(key, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0) {
    password_ = value;
  } else {
    return PostgresDatabase::SetOption(key, value, error);
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode MotherduckDatabase::ConnectParams(
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

  // Always pass the user, token and application name, so libpq never falls
  // back to PGUSER, PGPASSWORD, a password file, or PGAPPNAME.
  params->clear();
  std::string options = GetEnv("PGOPTIONS");
  std::string password;
  std::string user;
  std::string application_name;
  std::string fallback_application_name;
  // Connection settings from the URI that the region and sslmode interact with.
  std::string uri_host;
  std::string uri_hostaddr;
  std::string uri_port;
  std::string uri_sslmode;
  std::string uri_sslrootcert;
  std::string uri_service;
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    std::string_view keyword = option->keyword;
    if (keyword == "host") uri_host = option->val;
    if (keyword == "hostaddr") uri_hostaddr = option->val;
    if (keyword == "port") uri_port = option->val;
    if (keyword == "sslmode") uri_sslmode = option->val;
    if (keyword == "sslrootcert") uri_sslrootcert = option->val;
    if (keyword == "service") uri_service = option->val;
    if (keyword == "options") {
      options = option->val;
    } else if (keyword == "password") {
      password = option->val;
    } else if (keyword == "user") {
      user = option->val;
    } else if (keyword == "application_name") {
      application_name = option->val;
    } else if (keyword == "fallback_application_name") {
      fallback_application_name = option->val;
    } else {
      params->emplace_back(option->keyword, option->val);
    }
  }
  PQconninfoFree(conninfo);

  bool names_service = !uri_service.empty() || !GetEnv("PGSERVICE").empty();
  // On Windows the driver reads the process environment, but libpq reads its
  // own C runtime copy, which can be stale. Pass the TLS settings the driver
  // acts on, so libpq uses the same ones. With a named service the driver
  // doesn't act on them, and passing them would make them override the
  // service file's, which libpq ranks above the environment.
  if (!names_service) {
    if (uri_sslmode.empty()) {
      if (std::string env = GetEnv("PGSSLMODE"); !env.empty()) {
        params->emplace_back("sslmode", std::move(env));
      }
    }
    if (uri_sslrootcert.empty()) {
      if (std::string env = GetEnv("PGSSLROOTCERT"); !env.empty()) {
        params->emplace_back("sslrootcert", std::move(env));
      }
    }
  }

  if (!region_.empty()) {
    // A different host, or any hostaddr (which libpq connects to instead of the
    // host), would send the connection elsewhere.
    std::string region_host = RegionHost(region_);
    if ((!uri_host.empty() && NormalizeHost(uri_host) != region_host) ||
        !uri_hostaddr.empty() || !GetEnv("PGHOSTADDR").empty()) {
      InternalAdbcSetError(error,
                           "[libpq] Set either database option '%s' or a different "
                           "host (host or hostaddr in the URI, or PGHOSTADDR), not both",
                           ADBC_MOTHERDUCK_OPTION_REGION);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // libpq fills unset settings, including hostaddr, from a connection service.
    if (names_service) {
      InternalAdbcSetError(error,
                           "[libpq] Database option '%s' can't be used with a "
                           "connection service (service in the URI, or PGSERVICE)",
                           ADBC_MOTHERDUCK_OPTION_REGION);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (uri_host.empty()) params->emplace_back("host", std::move(region_host));
    // Like PGHOST, PGPORT is meant for some other server.
    if (uri_port.empty()) params->emplace_back("port", "5432");
  }
  // Don't let libpq's default sslmode=prefer send the token in plaintext.
  // Certificate verification still needs sslmode=verify-full (#34). A named
  // service may set sslmode itself, which this would override; with a region,
  // a service is already an error.
  if (!names_service && DefaultsToRequireSsl(uri_sslmode, uri_sslrootcert)) {
    params->emplace_back("sslmode", "require");
  }
  if (!password_.empty()) password = password_;
  if (password.empty()) password = GetEnv("MOTHERDUCK_TOKEN");
  if (password.empty()) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] No MotherDuck token: set the 'password' (or "
                         "'adbc.motherduck.token') database option, a password in "
                         "the URI, or MOTHERDUCK_TOKEN");
    return ADBC_STATUS_UNAUTHENTICATED;
  }
  params->emplace_back("password", std::move(password));
  if (!username_.empty()) user = username_;
  if (user.empty()) user = kDefaultUser;
  params->emplace_back("user", std::move(user));
  // Only an option appended after them can be affected.
  if ((!attach_mode_.empty() || !session_name_.empty()) &&
      EndsWithLoneBackslash(options)) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] Session options (options in the URI, or "
                         "PGOPTIONS) end in a backslash that escapes nothing");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // MotherDuck keeps the last value of a repeated setting, so defaults go
  // before the URI's options (or PGOPTIONS) and explicit options after them.
  std::string session_options;
  auto add = [&session_options](std::string_view session_option) {
    if (session_option.empty()) return;
    if (!session_options.empty()) session_options += ' ';
    session_options += session_option;
  };
  if (attach_mode_.empty()) add(SessionOption("attach_mode", kDefaultAttachMode));
  add(options);
  if (!attach_mode_.empty()) add(SessionOption("attach_mode", attach_mode_));
  if (!session_name_.empty()) add(SessionOption("session_name", session_name_));
  params->emplace_back("options", std::move(session_options));
  // MotherDuck's user agent comes from application_name: append the driver to
  // the application's name. Always passing it means PGAPPNAME is never used.
  if (application_name.empty()) application_name = fallback_application_name;
  if (!application_name.empty()) application_name += ' ';
  application_name += "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  params->emplace_back("application_name", std::move(application_name));
  return ADBC_STATUS_OK;
}

AdbcStatusCode MotherduckDatabase::Connect(PGconn** conn, struct AdbcError* error) {
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

}  // namespace adbcpq
