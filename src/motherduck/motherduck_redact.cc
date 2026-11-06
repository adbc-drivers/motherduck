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

#include <regex>

#include "error.h"

namespace adbcpq {

std::string RedactSecretStatements(std::string_view query) {
  static const std::regex kSecretStatement(
      R"((CREATE|ALTER)\s+(OR\s+REPLACE\s+)?((PERSISTENT|TEMPORARY)\s+)?SECRET\b)",
      std::regex::icase | std::regex::optimize);
  const std::string text(query);
  std::smatch match;
  if (!std::regex_search(text, match, kSecretStatement)) {
    return text;
  }
  const size_t paren = text.find('(', match.position(0) + match.length(0));
  if (paren == std::string::npos) {
    return text;
  }
  return text.substr(0, paren) + "(...)";
}

void InstallMotherDuckQueryRedactor() { SetQueryRedactor(&RedactSecretStatements); }

}  // namespace adbcpq
