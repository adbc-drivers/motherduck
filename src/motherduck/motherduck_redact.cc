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

#include <algorithm>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "error.h"

namespace adbcpq {

namespace {

// Replaces the statement echo that StripStatementEcho removes.
constexpr char kStatementOmitted[] = "[statement omitted]";

// Deliberately crude: patterns match raw text, ignoring quoting, and the earliest match
// cuts the text there (group 1 is what's kept). Over-masking is fine; leaking is not.
// Repeats are bounded because libstdc++'s matcher recurses once per iteration.

std::vector<std::regex> BuildPatterns() {
  constexpr auto kFlags = std::regex::ECMAScript | std::regex::icase;
  // Words that mark a name as a credential. The short ones must be a whole part of the
  // name, so "author" and "design" don't count. "key" must end a longer name (api_key,
  // sslkey); a bare key is handled below.
  const std::string credential =
      "(?:(?:secret|token|pass|signature|bearer|jwt|cookie|connection_string|"
      "[a-z0-9_-]key)"
      "|(?:^|[^a-z])(?:pwd|pw|sig|sas|auth|authorization|credentials?|slt|dsn)(?![a-z])"
      ")";
  // What may follow the name: '=', ':' or TO, or straight away a string, '(', ')', ','
  // or a comment. A double quote counts only after a space, so `Table "tokens" does not
  // exist` stays readable while PASSWORD "v" is masked.
  const std::string value =
      R"((?:["']?\s*(?:=|:(?!:)|\bto\b)|\s*(?:['(),]|/\*|--)|\s+"))";
  return std::vector<std::regex>{
      // CREATE [OR REPLACE] [PERSISTENT] SECRET <name> (...): the name goes too.
      std::regex(R"((\b(?:create|alter)\b[\s\S]{0,200}?\bsecret\b))", kFlags),
      // name = v, name TO v, name: v, PASSWORD 'v', name(v), (name) VALUES (v).
      std::regex("(" + credential + R"([\w.$]{0,64}))" + value, kFlags),
      // A bare key, only when a value follows directly: key = v, "key": v, KEY 'v'.
      std::regex(R"((\bkey)(?:["']?\s*(?:=|:(?!:)|\bto\b)|\s*'))", kFlags),
      // scheme://user:password@host. Text may end inside the password, once truncated.
      std::regex(R"((://[^\s'"@/]{0,128}:\s*)[^\s'"@]{0,1024}(?:@|$))", kFlags),
      // md:<token>@database, motherduck:<token>@database
      std::regex(R"((\b(?:md|motherduck):)[^'"@]{0,1024}(?:@|$))", kFlags),
      // md_create_flight(source_code := ..., config => ...)
      std::regex(R"((\b(?:source_code|config))\s*(?::=|=>?|/\*|--))", kFlags),
  };
}

// Built once; null if building failed.
const std::vector<std::regex>* Patterns() {
  static const std::vector<std::regex>* patterns =
      []() -> const std::vector<std::regex>* {
    try {
      static const std::vector<std::regex> built = BuildPatterns();
      return &built;
    } catch (...) {
      return nullptr;
    }
  }();
  return patterns;
}

}  // namespace

std::string RedactSecretStatements(std::string_view query) {
  if (query.empty()) return std::string();
  // Runs while an error is built, so it must not throw: on failure, hide everything.
  const std::vector<std::regex>* patterns = Patterns();
  if (patterns == nullptr) return "(redacted)";
  try {
    const char* begin = query.data();
    const char* end = begin + query.size();
    size_t cut = query.size();
    bool found = false;
    for (const std::regex& pattern : *patterns) {
      std::cmatch match;
      if (!std::regex_search(begin, end, match, pattern)) continue;
      cut = std::min(cut, static_cast<size_t>(match.position(1) + match.length(1)));
      found = true;
    }
    if (!found) return std::string(query);
    return std::string(query.substr(0, cut)) + " ...";
  } catch (...) {
    return "(redacted)";
  }
}

std::string StripStatementEcho(std::string_view text) {
  // Find a line starting with "LINE", spaces, digits and ':'. Text cut short inside
  // that header has no statement after it yet, so leaving it is harmless.
  for (size_t line = 0; line < text.size();) {
    size_t i = line;
    if (text.compare(i, 4, "LINE") == 0) {
      i += 4;
      while (i < text.size() && text[i] == ' ') i++;
      const size_t digits = i;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') i++;
      if (i > digits && i < text.size() && text[i] == ':') {
        size_t cut = line;
        while (cut > 0 && (text[cut - 1] == '\n' || text[cut - 1] == '\r')) cut--;
        std::string out(text.substr(0, cut));
        if (!out.empty()) out += ' ';
        return out + kStatementOmitted;
      }
    }
    const size_t next = text.find('\n', line);
    if (next == std::string_view::npos) break;
    line = next + 1;
  }
  return std::string(text);
}

namespace {

// The message redactor. StripStatementEcho can throw bad_alloc, and an error is being
// built, so this must not throw: on failure, hide everything.
std::string RedactServerMessage(std::string_view text) {
  try {
    return RedactSecretStatements(StripStatementEcho(text));
  } catch (...) {
    return "(redacted)";
  }
}

}  // namespace

void InstallMotherDuckRedactors() {
  // Message first: until it is set, messages fall back to the query redactor.
  SetMessageRedactor(&RedactServerMessage);
  SetQueryRedactor(&RedactSecretStatements);
}

}  // namespace adbcpq
