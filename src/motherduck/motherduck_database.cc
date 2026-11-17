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
#include "error.h"
#include "motherduck_ascii.h"
#include "motherduck_connection.h"
#include "motherduck_trust_store.h"
#include "motherduck_type_resolver.h"

namespace adbcpq {

namespace {
constexpr char kDefaultAttachMode[] = "single";
// MotherDuck's endpoint expects the user postgres.
constexpr char kDefaultUser[] = "postgres";
// The URI schemes the driver accepts, and the only ones, so it doesn't commit to
// libpq's connection syntax. An ADBC driver manager selects this driver by
// motherduck://; md:// is the short form. Matched case-sensitively, like libpq
// matches postgresql://.
constexpr std::string_view kMotherduckSchemes[] = {"motherduck://", "md://"};

// The length of the accepted scheme uri starts with, or 0 if it starts with none.
size_t MotherduckSchemeLength(std::string_view uri) {
  for (std::string_view scheme : kMotherduckSchemes) {
    if (uri.starts_with(scheme)) return scheme.size();
  }
  return 0;
}

// Decodes %XX sequences. False for a malformed sequence, or %00, which would end
// the C string the result is used as. libpq's own URI parser rejects %00 too.
bool PercentDecode(std::string_view in, std::string* out) {
  auto hex = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out->clear();
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] != '%') {
      *out += in[i];
      continue;
    }
    if (i + 2 >= in.size()) return false;
    int high = hex(in[i + 1]);
    int low = hex(in[i + 2]);
    if (high < 0 || low < 0) return false;
    // A NUL would end the C string the value is validated and used as.
    if (high == 0 && low == 0) return false;
    *out += static_cast<char>(high * 16 + low);
    i += 2;
  }
  return true;
}

// DuckDB's md:[database][?parameters] form, which has no slashes. md: alone names
// the workspace.
constexpr std::string_view kShortFormPrefix = "md:";
constexpr std::string_view kWorkspaceDatabase = "md%3A";

// A database name has no '/' or '@', even percent-encoded: md:host/db is a
// mistyped md://host/db, and md:token@db is DuckDB's token form, which would send
// the token to the server as a database name. The name must also decode.
bool IsShortForm(std::string_view uri) {
  if (!uri.starts_with(kShortFormPrefix)) return false;
  std::string_view database = uri.substr(kShortFormPrefix.size());
  database = database.substr(0, database.find('?'));
  std::string decoded;
  return PercentDecode(database, &decoded) &&
         database.find_first_of("/@") == std::string_view::npos &&
         decoded.find_first_of("/@") == std::string::npos;
}

bool IsAcceptedUri(std::string_view uri) {
  return MotherduckSchemeLength(uri) > 0 || IsShortForm(uri);
}

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

// Whether value is empty or a whole number of seconds, which fits in an int.
bool IsTimeoutSeconds(std::string_view value) {
  for (char c : value) {
    if (c < '0' || c > '9') return false;
  }
  // Leading zeros don't count towards the limit.
  size_t digits = value.find_first_not_of('0');
  return digits == std::string_view::npos || value.size() - digits <= 9;
}

// The settings a URI query parameter can set, named like the database options.
// motherduck.token is left out so a token doesn't end up in a URI's query, which
// is more likely to be logged than its password.
constexpr std::string_view kUriSettingKeys[] = {
    ADBC_MOTHERDUCK_OPTION_ATTACH_MODE,
    ADBC_MOTHERDUCK_OPTION_SESSION_NAME,
    ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME,
    ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT,
    ADBC_MOTHERDUCK_OPTION_REGION,
    ADBC_MOTHERDUCK_OPTION_DB_SCHEMA,
    ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH,
    ADBC_MOTHERDUCK_OPTION_TLS_VERIFY,
};

bool IsUriSettingKey(std::string_view key) {
  for (std::string_view name : kUriSettingKeys) {
    if (key == name) return true;
  }
  return false;
}

// Sets the setting named key, after validating value, which is a database
// option's or a URI query parameter's (from_uri). An empty value unsets it.
// NOT_FOUND, with no error set, if key names none of them. A rejected URI value
// isn't repeated in the error, in case it's part of a token.
AdbcStatusCode SetSetting(MotherduckSettings* settings, const char* key,
                          const char* value, bool from_uri, struct AdbcError* error) {
  auto invalid = [&](const char* expected) {
    if (from_uri) {
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': invalid value for query "
                           "parameter %s: expected %s",
                           key, expected);
    } else {
      InternalAdbcSetError(error, "[libpq] Invalid value for option %s=%s: expected %s",
                           key, value, expected);
    }
    return ADBC_STATUS_INVALID_ARGUMENT;
  };
  if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    // Unset, it's the URI's, or single.
    if (*value != '\0' && std::strcmp(value, "single") != 0 &&
        std::strcmp(value, "workspace") != 0) {
      return invalid("single or workspace");
    }
    settings->attach_mode = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    settings->session_name = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME) == 0) {
    settings->application_name = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT) == 0) {
    // A whole number of seconds; 0 waits indefinitely.
    if (!IsTimeoutSeconds(value)) {
      return invalid("a whole number of seconds from 0 to 999999999, such as 10");
    }
    // Read back the same however it was written: 007 is 7.
    settings->connect_timeout = *value == '\0' ? "" : std::to_string(std::stoll(value));
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    // The region becomes part of a hostname.
    std::string region = AsciiLower(value);
    if (!region.empty() && RegionHost(region).empty()) {
      return invalid(
          "a region such as us-east-1 or eastus2, of letters, digits and dashes");
    }
    settings->region = std::move(region);
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA) == 0) {
    settings->db_schema = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH) == 0) {
    // The file is checked when connecting.
    settings->tls_root_cert_path = value;
  } else if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY) == 0) {
    if (*value == '\0') {
      // Unset, so the URI's value, or the default (true), applies.
      settings->tls_verify.reset();
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {
      settings->tls_verify = true;
    } else if (std::strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {
      settings->tls_verify = false;
    } else {
      return invalid("true or false");
    }
  } else {
    return ADBC_STATUS_NOT_FOUND;
  }
  return ADBC_STATUS_OK;
}

// Splits uri, already accepted by IsAcceptedUri, into the URI libpq parses, which
// has no query, and the settings its query parameters set. The query takes only
// the settings named like database options; anything else, including libpq's own
// parameters, is an error, and the driver never passes the query on to libpq.
AdbcStatusCode ParseUri(std::string_view uri, std::string* libpq_uri,
                        MotherduckSettings* settings, struct AdbcError* error) {
  std::string normalized = NormalizeUri(uri);
  size_t query_start = normalized.find('?');
  *libpq_uri = normalized.substr(0, query_start);
  // Fail now, not when connecting, on a host, port or percent-encoding libpq
  // rejects. Its message quotes the URI, which can hold a token, so isn't used.
  {
    char* parse_error = nullptr;
    PQconninfoOption* parsed = PQconninfoParse(libpq_uri->c_str(), &parse_error);
    PQfreemem(parse_error);
    if (parsed == nullptr) {
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': the URI doesn't parse; check "
                           "its host, port and percent-encoding");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    PQconninfoFree(parsed);
  }
  if (query_start == std::string::npos) return ADBC_STATUS_OK;
  std::string_view query = std::string_view(normalized).substr(query_start + 1);
  while (!query.empty()) {
    size_t end = query.find('&');
    std::string_view parameter = query.substr(0, end);
    query = end == std::string_view::npos ? std::string_view() : query.substr(end + 1);
    if (parameter.empty()) continue;
    size_t equals = parameter.find('=');
    std::string key, value;
    if (!PercentDecode(parameter.substr(0, equals), &key) ||
        (equals != std::string_view::npos &&
         !PercentDecode(parameter.substr(equals + 1), &value))) {
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': a query parameter isn't "
                           "valid percent-encoding");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // Without an '=', this may be part of a token with an unencoded '?' in it.
    if (equals == std::string_view::npos) {
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': a query parameter has no "
                           "value, or the URI has an unencoded '?'");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (!IsUriSettingKey(key)) {
      std::string supported;
      for (std::string_view name : kUriSettingKeys) {
        if (!supported.empty()) supported += ", ";
        supported += name;
      }
      // The parameter isn't named: with an unencoded '?' in a password, what
      // follows it looks like one.
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': unsupported query parameter. "
                           "The URI's query takes only the options %s",
                           supported.c_str());
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // IsUriSettingKey checked the key, so this only fails on the value.
    RAISE_ADBC(
        SetSetting(settings, key.c_str(), value.c_str(), /*from_uri=*/true, error));
  }
  return ADBC_STATUS_OK;
}

// Backslash-escapes whitespace and backslashes for libpq options. MotherDuck
// removes the escapes, so "a b" arrives as is.
std::string EscapeOptionValue(std::string_view value) {
  std::string escaped;
  for (char c : value) {
    // ASCII whitespace only: isspace follows the host application's locale, and
    // could escape a byte of a multi-byte character.
    if (c == '\\' || c == ' ' || (c >= '\t' && c <= '\r')) escaped += '\\';
    escaped += c;
  }
  return escaped;
}
}  // namespace

std::string NormalizeUri(std::string_view uri) {
  if (size_t length = MotherduckSchemeLength(uri); length > 0) {
    return "postgresql://" + std::string(uri.substr(length));
  }
  if (IsShortForm(uri)) {
    std::string_view rest = uri.substr(kShortFormPrefix.size());
    // The database, then any parameters. An empty database is the workspace.
    size_t query = rest.find('?');
    std::string_view database = rest.substr(0, query);
    std::string_view parameters =
        query == std::string_view::npos ? std::string_view() : rest.substr(query);
    return "postgresql:///" +
           std::string(database.empty() ? kWorkspaceDatabase : database) +
           std::string(parameters);
  }
  return std::string(uri);
}

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

bool DriverChoosesSslmode() {
  if (!GetEnv("PGSSLMODE").empty()) return false;
  // With sslrootcert=system, libpq already defaults to verify-full and rejects
  // anything weaker.
  return GetEnv("PGSSLROOTCERT") != "system";
}

std::string ScrubConnectError(std::string_view message, std::string_view user,
                              std::string_view password) {
  constexpr std::string_view kRedacted = "<redacted>";
  // The server and libpq quote the user, which is a token if it was put in the
  // user's place. The default user isn't a secret, and stays readable.
  std::string_view secrets[] = {password, user == kDefaultUser ? "" : user};
  // One pass over the original message, so a replacement is never searched
  // again, and the longest secret wins where two start at the same place.
  std::string scrubbed;
  for (size_t i = 0; i < message.size();) {
    size_t match = 0;
    for (std::string_view secret : secrets) {
      if (secret.size() > match && message.substr(i).starts_with(secret)) {
        match = secret.size();
      }
    }
    if (match > 0) {
      scrubbed += kRedacted;
      i += match;
    } else {
      scrubbed += message[i++];
    }
  }
  return scrubbed;
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

MotherduckSettings MotherduckDatabase::Settings() const {
  MotherduckSettings settings = option_settings_;
  auto fill = [](std::string* value, const std::string& fallback) {
    if (value->empty()) *value = fallback;
  };
  fill(&settings.attach_mode, uri_settings_.attach_mode);
  fill(&settings.session_name, uri_settings_.session_name);
  fill(&settings.application_name, uri_settings_.application_name);
  fill(&settings.connect_timeout, uri_settings_.connect_timeout);
  fill(&settings.region, uri_settings_.region);
  fill(&settings.db_schema, uri_settings_.db_schema);
  fill(&settings.tls_root_cert_path, uri_settings_.tls_root_cert_path);
  if (!settings.tls_verify) settings.tls_verify = uri_settings_.tls_verify;
  return settings;
}

AdbcStatusCode MotherduckDatabase::GetOption(const char* option, char* value,
                                             size_t* length, struct AdbcError* error) {
  std::string output;
  const MotherduckSettings settings = Settings();
  // Options that are unset when empty.
  const std::string* optional = nullptr;
  if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_ATTACH_MODE) == 0) {
    output = settings.attach_mode.empty() ? kDefaultAttachMode : settings.attach_mode;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_SESSION_NAME) == 0) {
    optional = &settings.session_name;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_APPLICATION_NAME) == 0) {
    optional = &settings.application_name;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT) == 0) {
    optional = &settings.connect_timeout;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_REGION) == 0) {
    optional = &settings.region;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA) == 0) {
    optional = &settings.db_schema;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH) == 0) {
    optional = &settings.tls_root_cert_path;
  } else if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY) == 0) {
    output = settings.tls_verify.value_or(true) ? ADBC_OPTION_VALUE_ENABLED
                                                : ADBC_OPTION_VALUE_DISABLED;
  } else if (std::strcmp(option, ADBC_OPTION_USERNAME) == 0) {
    // The user the driver connects as.
    output = username_;
    // uri_ was validated when set, so it parses.
    if (output.empty() && !uri_.empty()) UriValue(uri_, "user", &output);
    if (output.empty()) output = kDefaultUser;
  } else if (std::strcmp(option, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(option, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0 ||
             std::strcmp(option, "uri") == 0) {
    // The URI may hold a token.
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

AdbcStatusCode MotherduckDatabase::GetOptionInt(const char* option, int64_t* value,
                                                struct AdbcError* error) {
  if (std::strcmp(option, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT) == 0) {
    if (value == nullptr) {
      InternalAdbcSetError(error, "[libpq] Option %s requires an output pointer", option);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    const std::string timeout = Settings().connect_timeout;
    if (timeout.empty()) {
      InternalAdbcSetError(error, "[libpq] database option '%s' is not set", option);
      return ADBC_STATUS_NOT_FOUND;
    }
    *value = std::stoll(timeout);
    return ADBC_STATUS_OK;
  }
  return PostgresDatabase::GetOptionInt(option, value, error);
}

AdbcStatusCode MotherduckDatabase::SetOptionInt(const char* key, int64_t value,
                                                struct AdbcError* error) {
  if (std::strcmp(key, ADBC_MOTHERDUCK_OPTION_CONNECT_TIMEOUT) == 0) {
    // Languages like Python send a number through the integer setter.
    return SetOption(key, std::to_string(value).c_str(), error);
  }
  return PostgresDatabase::SetOptionInt(key, value, error);
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
    return ADBC_STATUS_OK;
  }
  // Database options set the settings that a URI's query parameters also can. An
  // empty value unsets them, which lets the URI's value apply again.
  AdbcStatusCode status =
      SetSetting(&option_settings_, key, value, /*from_uri=*/false, error);
  if (status != ADBC_STATUS_NOT_FOUND) return status;
  if (std::strcmp(key, ADBC_OPTION_USERNAME) == 0) {
    username_ = value;
  } else if (std::strcmp(key, ADBC_OPTION_PASSWORD) == 0 ||
             std::strcmp(key, ADBC_MOTHERDUCK_OPTION_TOKEN) == 0) {
    password_ = value;
  } else if (std::strcmp(key, "uri") == 0) {
    // An empty value unsets it.
    if (*value == '\0') {
      uri_settings_ = {};
      return PostgresDatabase::SetOption(key, value, error);
    }
    if (!IsAcceptedUri(value)) {
      InternalAdbcSetError(error,
                           "Invalid database option 'uri': expected a "
                           "motherduck://, md:// or md: URI, such as "
                           "motherduck:///my_db, "
                           "md://pg.us-east-1-aws.motherduck.com/my_db or md:my_db");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // Nothing changes unless the whole URI is valid. What libpq gets has no query,
    // so nothing that reads uri_ sees motherduck://, md://, md: or the settings.
    std::string libpq_uri;
    MotherduckSettings uri_settings;
    RAISE_ADBC(ParseUri(value, &libpq_uri, &uri_settings, error));
    RAISE_ADBC(PostgresDatabase::SetOption(key, libpq_uri.c_str(), error));
    uri_settings_ = std::move(uri_settings);
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
    // uri_ was validated when set, so this is out of memory. libpq's message
    // would quote the URI, which can hold a token.
    PQfreemem(parse_error);
    InternalAdbcSetError(error, "%s", "[libpq] Failed to connect: out of memory");
    return ADBC_STATUS_IO;
  }

  // The URI's query isn't passed on, so it carries only the user, token, host,
  // port and database. Always pass the user, token and application name, so libpq
  // never falls back to PGUSER, PGPASSWORD, a password file, or PGAPPNAME.
  params->clear();
  const MotherduckSettings settings = Settings();
  std::string options = GetEnv("PGOPTIONS");
  std::string password;
  std::string user;
  // Connection settings from the URI that the region interacts with.
  std::string uri_host;
  std::string uri_port;
  for (PQconninfoOption* option = conninfo; option->keyword != nullptr; option++) {
    if (option->val == nullptr) continue;
    std::string_view keyword = option->keyword;
    if (keyword == "host") uri_host = option->val;
    if (keyword == "port") uri_port = option->val;
    if (keyword == "password") {
      password = option->val;
    } else if (keyword == "user") {
      user = option->val;
    } else {
      params->emplace_back(option->keyword, option->val);
    }
  }
  PQconninfoFree(conninfo);

  // The TLS options settle sslmode and sslrootcert themselves. Passed
  // explicitly, they outrank PGSSLMODE, PGSSLROOTCERT and a connection service
  // file.
  const bool tls_verify = settings.tls_verify.value_or(true);
  const bool pinned_roots = !settings.tls_root_cert_path.empty();
  const bool tls_options = pinned_roots || !tls_verify;
  if (tls_options) {
    if (pinned_roots && !tls_verify) {
      InternalAdbcSetError(error, "[libpq] Set either option '%s' or %s=%s, not both",
                           ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH,
                           ADBC_MOTHERDUCK_OPTION_TLS_VERIFY, ADBC_OPTION_VALUE_DISABLED);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (pinned_roots && !IsReadableFile(settings.tls_root_cert_path)) {
      InternalAdbcSetError(error, "[libpq] Can't read option '%s': %s",
                           ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH,
                           settings.tls_root_cert_path.c_str());
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (pinned_roots) params->emplace_back("sslrootcert", settings.tls_root_cert_path);
    params->emplace_back("sslmode", pinned_roots ? "verify-full" : "require");
  } else {
    // On Windows the driver reads the process environment, but libpq reads
    // its own C runtime copy, which can be stale. Pass the TLS settings the
    // driver acts on, so libpq uses the same ones.
    if (std::string env = GetEnv("PGSSLMODE"); !env.empty()) {
      params->emplace_back("sslmode", std::move(env));
    }
    if (std::string env = GetEnv("PGSSLROOTCERT"); !env.empty()) {
      params->emplace_back("sslrootcert", std::move(env));
    }
  }

  if (!settings.region.empty()) {
    // A different host, or any hostaddr (which libpq connects to instead of the
    // host), would send the connection elsewhere.
    std::string region_host = RegionHost(settings.region);
    if ((!uri_host.empty() && NormalizeHost(uri_host) != region_host) ||
        !GetEnv("PGHOSTADDR").empty()) {
      InternalAdbcSetError(error,
                           "[libpq] Set either option '%s' or a different host (in the "
                           "URI, or PGHOSTADDR), not both",
                           ADBC_MOTHERDUCK_OPTION_REGION);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // libpq fills unset settings, including hostaddr, from a connection service.
    if (!GetEnv("PGSERVICE").empty()) {
      InternalAdbcSetError(error,
                           "[libpq] Option '%s' can't be used with a connection "
                           "service (PGSERVICE)",
                           ADBC_MOTHERDUCK_OPTION_REGION);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (uri_host.empty()) params->emplace_back("host", std::move(region_host));
    // Like PGHOST, PGPORT is meant for some other server.
    if (uri_port.empty()) params->emplace_back("port", "5432");
  }
  // Don't let libpq's default sslmode=prefer send the token in plaintext, and
  // verify the server's certificate. A named service may set sslmode and
  // sslrootcert itself, which defaults here would override; with a region, a
  // service is already an error.
  bool names_service = !GetEnv("PGSERVICE").empty();
  if (!tls_options && !names_service) {
    std::string sslmode = GetEnv("PGSSLMODE");
    std::string sslrootcert = GetEnv("PGSSLROOTCERT");
    // libpq's own default root certificate file is the user's choice too.
    if (sslrootcert.empty() && IsReadableFile(DefaultRootCertPath(GetEnv))) {
      sslrootcert = DefaultRootCertPath(GetEnv);
    }
    // libpq can't find the OS's trusted roots (sslrootcert=system needs a
    // libpq and OpenSSL that can), so pass them as a file. Only for modes that
    // check the host name: verify-ca with every public CA would accept any
    // public certificate for any host, and a root file would make require
    // verify the CA.
    //
    // Without root certificates the connection fails rather than falling back
    // to an unverified one, which would send the token to whoever answers.
    // tls_verify=false is the explicit way to skip verification.
    if (sslrootcert.empty() && (sslmode.empty() || sslmode == "verify-full")) {
      // A bundle the user named but that can't be read is a mistake to report,
      // not a reason to look elsewhere.
      if (std::string cert_file = GetEnv("SSL_CERT_FILE");
          !cert_file.empty() && !IsReadableFile(cert_file)) {
        InternalAdbcSetError(error,
                             "[libpq] Can't read the CA bundle SSL_CERT_FILE names: %s. "
                             "Fix or unset it, or set database option '%s' to a PEM "
                             "CA bundle",
                             cert_file.c_str(),
                             ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH);
        return ADBC_STATUS_INVALID_STATE;
      }
      sslrootcert = find_ca_bundle_ ? find_ca_bundle_() : FindCaBundle(GetEnv);
      if (sslrootcert.empty()) {
        InternalAdbcSetError(
            error,
            "[libpq] No trusted CA certificates found to verify the server's "
            "certificate. Install the OS's CA certificates (such as the "
            "ca-certificates package), set database option '%s' (or SSL_CERT_FILE) "
            "to a PEM CA bundle, or set %s=false to connect without verifying",
            ADBC_MOTHERDUCK_OPTION_TLS_ROOT_CERT_PATH, ADBC_MOTHERDUCK_OPTION_TLS_VERIFY);
        return ADBC_STATUS_INVALID_STATE;
      }
      params->emplace_back("sslrootcert", sslrootcert);
    }
    if (DriverChoosesSslmode()) {
      params->emplace_back("sslmode", "verify-full");
    }
  }
  if (!password_.empty()) password = password_;
  if (password.empty()) password = GetEnv("MOTHERDUCK_TOKEN");
  if (password.empty()) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] No MotherDuck token: set the 'password' (or "
                         "'motherduck.token') database option, a password in "
                         "the URI, or MOTHERDUCK_TOKEN");
    return ADBC_STATUS_UNAUTHENTICATED;
  }
  params->emplace_back("password", std::move(password));
  if (!username_.empty()) user = username_;
  if (user.empty()) user = kDefaultUser;
  params->emplace_back("user", std::move(user));
  // Only an option appended after them can be affected.
  if ((!settings.attach_mode.empty() || !settings.session_name.empty()) &&
      EndsWithLoneBackslash(options)) {
    InternalAdbcSetError(error, "%s",
                         "[libpq] Session options (PGOPTIONS) end in a backslash that "
                         "escapes nothing");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // MotherDuck keeps the last value of a repeated setting, so defaults go
  // before PGOPTIONS and explicit options after them.
  std::string session_options;
  auto add = [&session_options](std::string_view session_option) {
    if (session_option.empty()) return;
    if (!session_options.empty()) session_options += ' ';
    session_options += session_option;
  };
  // Have the endpoint send PostgreSQL command tags with affected-row counts
  // instead of DuckDB's Count and Success result sets.
  add(SessionOption("return_pg_command_tags", "on"));
  if (settings.attach_mode.empty()) {
    add(SessionOption("attach_mode", kDefaultAttachMode));
  }
  add(options);
  if (!settings.attach_mode.empty()) {
    add(SessionOption("attach_mode", settings.attach_mode));
  }
  if (!settings.session_name.empty()) {
    add(SessionOption("session_name", settings.session_name));
  }
  params->emplace_back("options", std::move(session_options));
  // MotherDuck's user agent comes from application_name: append the driver to
  // the application's name. Always passing it means PGAPPNAME is never used.
  std::string application_name = settings.application_name;
  // PostgreSQL cuts names at 63 bytes, which would cut the driver's identifier
  // off, so shorten the application's name to leave room for it.
  constexpr std::string_view kDriverName =
      "adbc_driver_motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION;
  constexpr size_t kMaxApplicationNameBytes = 63;
  size_t name_budget = kMaxApplicationNameBytes > kDriverName.size() + 1
                           ? kMaxApplicationNameBytes - kDriverName.size() - 1
                           : 0;
  if (application_name.size() > name_budget) {
    // Cut before a character, not in the middle of one: back up while the first
    // byte dropped is a continuation byte.
    size_t cut = name_budget;
    while (cut > 0 &&
           (static_cast<unsigned char>(application_name[cut]) & 0xC0) == 0x80) {
      cut--;
    }
    application_name.resize(cut);
  }
  if (!application_name.empty()) application_name += ' ';
  application_name += kDriverName;
  params->emplace_back("application_name", std::move(application_name));
  if (!settings.connect_timeout.empty()) {
    params->emplace_back("connect_timeout", settings.connect_timeout);
  }
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
    std::string user, password;
    for (const auto& [keyword, value] : params) {
      if (keyword == "user") user = value;
      if (keyword == "password") password = value;
    }
    InternalAdbcSetError(
        error, "%s%s", "[libpq] Failed to connect: ",
        RedactMessage(ScrubConnectError(PQerrorMessage(*conn), user, password)).c_str());
    PQfinish(*conn);
    *conn = nullptr;
    return ADBC_STATUS_IO;
  }

  // The endpoint rejects search_path as a startup option, so set it once
  // connected. A database the URI names is the connection's current database,
  // which saves looking it up. Otherwise (md:, or no database, where libpq
  // falls back to the user name) the server says which it is.
  const std::string db_schema = Settings().db_schema;
  if (!db_schema.empty()) {
    std::string_view database;
    for (const auto& [keyword, value] : params) {
      if (keyword == "dbname" && !value.empty() && value.find(':') == std::string::npos) {
        database = value;
      }
    }
    AdbcStatusCode status = SetMotherduckSearchPath(
        *conn, db_schema, database, ADBC_MOTHERDUCK_OPTION_DB_SCHEMA, error);
    if (status != ADBC_STATUS_OK) {
      PQfinish(*conn);
      *conn = nullptr;
      return status;
    }
  }
  open_connections_++;
  return ADBC_STATUS_OK;
}

}  // namespace adbcpq
