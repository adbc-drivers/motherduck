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

// Deliberately crude: each pattern looks at raw text, ignoring quoting and comments, and
// the first match anywhere cuts the text there. Group 1 is the part kept. Over-masking is
// fine; leaking is not. Loops are bounded because the input is already truncated and
// libstdc++'s matcher recurses once per iteration.

std::vector<std::regex> BuildPatterns() {
  constexpr auto kFlags = std::regex::ECMAScript | std::regex::icase;
  // A word that marks a name as a credential, as part of the name (api_key, monkey).
  // The short words must be a whole part, so author and design don't count.
  const std::string credential =
      "(?:(?:secret|token|pass|key|signature|bearer|jwt|cookie|connection_string)"
      "|(?:^|[^a-z])(?:pwd|pw|sig|sas|auth|authorization|credentials?|slt|dsn)(?![a-z])"
      ")";
  // What may follow the name: '=', ':' or TO (after an optional closing quote of a
  // quoted name), or straight away a string, '(', ')', ',' or a comment hiding one. A
  // double quote only counts after a space, so `Table "tokens" does not exist` stays
  // readable while PASSWORD "v" is still masked.
  const std::string value =
      R"((?:["']?\s*(?:=|:(?!:)|\bto\b)|\s*(?:['(),]|/\*|--)|\s+"))";
  return std::vector<std::regex>{
      // CREATE [OR REPLACE] [PERSISTENT] SECRET <name> (options): drop the name too.
      std::regex(R"((\b(?:create|alter)\b[\s\S]{0,200}?\bsecret\b))", kFlags),
      // name = v, name TO v, name: v, PASSWORD 'v', name(v), (name) VALUES (v).
      std::regex("(" + credential + R"([\w.$]{0,64}))" + value, kFlags),
      // scheme://user:password@host
      std::regex(R"((://[^\s'"@/]{0,128}:\s*)[^\s'"@]{0,1024}@)", kFlags),
      // md:<token>@database, motherduck:<token>@database
      std::regex(R"((\b(?:md|motherduck):)[^'"@]{0,1024}@)", kFlags),
      // md_create_flight(source_code := ..., config => ...): free-form payloads.
      std::regex(R"((\b(?:source_code|config))\s*(?::=|=>?|/\*|--))", kFlags),
  };
}

// Built once. If building fails, null for good, rather than retrying on every error.
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
  // This runs while an error is being built, so it must not throw. If the patterns can't
  // be built or the matcher gives up, hide everything.
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

void InstallMotherDuckQueryRedactor() { SetQueryRedactor(&RedactSecretStatements); }

}  // namespace adbcpq
