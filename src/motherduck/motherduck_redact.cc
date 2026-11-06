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

// Masks credentials in SQL text that would otherwise appear in error messages.

#include "motherduck_redact.h"

#include <cctype>
#include <optional>
#include <string_view>

#include "error.h"

namespace adbcpq {

namespace {

// Deliberately crude: the scan looks at raw text and ignores quoting, so it also sees
// inside string literals, comments and $$ bodies. When it can't be sure, it masks more.

struct Cut {
  size_t at;  // keep text[0, at)
  const char* suffix;
};

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
         c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (std::toupper(static_cast<unsigned char>(a[i])) != b[i]) return false;
  }
  return true;  // b is upper case
}

bool EndsWithNoCase(std::string_view a, std::string_view upper_suffix) {
  return a.size() > upper_suffix.size() &&
         EqualsNoCase(a.substr(a.size() - upper_suffix.size()), upper_suffix);
}

// Whether any '_' or '.' separated part of the name is a credential word, so that
// s3_access_key_id counts but monkey and tokenizer don't. Run-together names such as
// AccountKey, sslpassword and apikey count too, by their ending.
bool IsCredentialName(std::string_view name) {
  for (size_t i = 0; i + 17 <= name.size(); i++) {
    if (EqualsNoCase(name.substr(i, 17), "CONNECTION_STRING")) return true;
  }
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find_first_of("_.$", start);
    if (end == std::string_view::npos) end = name.size();
    const std::string_view part = name.substr(start, end - start);
    for (const char* word : {"SECRET", "TOKEN", "PASSWORD", "PASSWD", "PASSPHRASE", "PWD",
                             "PW", "KEY", "SIG", "SIGNATURE", "SAS", "AUTH",
                             "AUTHORIZATION", "CREDENTIAL", "CREDENTIALS"}) {
      if (EqualsNoCase(part, word)) return true;
    }
    for (const char* word :
         {"SECRET", "TOKEN", "PASSWORD", "PASSWD", "PASSPHRASE", "KEY", "SIGNATURE"}) {
      if (EndsWithNoCase(part, word)) return true;
    }
    start = end + 1;
  }
  return false;
}

size_t SkipSpaceAndComments(std::string_view t, size_t i) {
  while (i < t.size()) {
    if (std::isspace(static_cast<unsigned char>(t[i]))) {
      i++;
    } else if (t.compare(i, 2, "/*") == 0) {
      const size_t end = t.find("*/", i + 2);
      i = end == std::string_view::npos ? t.size() : end + 2;
    } else if (t.compare(i, 2, "--") == 0) {
      const size_t end = t.find('\n', i);
      i = end == std::string_view::npos ? t.size() : end + 1;
    } else {
      break;
    }
  }
  return i;
}

// After a credential name at t[0, i): is a value about to follow (name = v, name TO v,
// name: v, name 'v')? A quoted name may have its closing quote first.
std::optional<Cut> ValueAfterName(std::string_view t, std::string_view name, size_t i) {
  if (i < t.size() && (t[i] == '\'' || t[i] == '"')) i++;
  const size_t k = SkipSpaceAndComments(t, i);
  if (k >= t.size()) return std::nullopt;
  const char c = t[k];
  if (c == '=') return Cut{k + 1, " ..."};
  if (c == ':' && !(k + 1 < t.size() && t[k + 1] == ':')) return Cut{k + 1, " ..."};
  if (c == '\'') return Cut{k, "..."};
  // add_parquet_key('name', 'key'). Not for a bare KEY, as in PRIMARY KEY (id).
  if (c == '(' && !EqualsNoCase(name, "KEY")) return Cut{k + 1, "...)"};
  if ((c == 't' || c == 'T') && k + 1 < t.size() &&
      (t[k + 1] == 'o' || t[k + 1] == 'O') &&
      !(k + 2 < t.size() && IsIdentChar(t[k + 2]))) {
    return Cut{k + 2, " ..."};
  }
  return std::nullopt;
}

// scheme://user:password@host: cut after the colon.
std::optional<Cut> UserinfoPassword(std::string_view t, size_t i) {
  size_t colon = std::string_view::npos;
  for (size_t j = i + 3; j < t.size(); j++) {
    if (std::string_view("/?#'\" \t\r\n").find(t[j]) != std::string_view::npos) break;
    if (t[j] == ':' && colon == std::string_view::npos) colon = j;
    if (t[j] == '@') {
      if (colon != std::string_view::npos) return Cut{colon + 1, "..."};
      break;
    }
  }
  return std::nullopt;
}

// Find the first place that needs masking. Everything after it is dropped.
std::optional<Cut> FindCut(std::string_view t) {
  bool create_or_alter_seen = false;
  size_t i = 0;
  while (i < t.size()) {
    if (!IsIdentChar(t[i])) {
      if (t.compare(i, 3, "://") == 0) {
        if (auto cut = UserinfoPassword(t, i)) return cut;
      }
      i++;
      continue;
    }
    const size_t start = i;
    while (i < t.size() && IsIdentChar(t[i])) i++;
    const std::string_view word = t.substr(start, i - start);

    if (EqualsNoCase(word, "CREATE") || EqualsNoCase(word, "ALTER")) {
      create_or_alter_seen = true;
    } else if (create_or_alter_seen && EqualsNoCase(word, "SECRET")) {
      // CREATE [OR REPLACE] [PERSISTENT|TEMPORARY] SECRET name (options)
      const size_t paren = t.find('(', i);
      if (paren == std::string_view::npos) return Cut{i, " ..."};
      return Cut{paren + 1, "...)"};
    }
    if (IsCredentialName(word)) {
      if (auto cut = ValueAfterName(t, word, i)) return cut;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string RedactSecretStatements(std::string_view query) {
  if (auto cut = FindCut(query)) {
    return std::string(query.substr(0, cut->at)) + cut->suffix;
  }
  return std::string(query);
}

void InstallMotherDuckQueryRedactor() { SetQueryRedactor(&RedactSecretStatements); }

}  // namespace adbcpq
