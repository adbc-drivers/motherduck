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

#pragma once

#include <string>
#include <string_view>

namespace adbcpq {

/// \brief Mask credentials in SQL text before it goes into an error message.
///
/// A few regexes over the raw text, ignoring quoting and comments. At the earliest match
/// the rest is dropped and " ..." appended. Triggers:
///  - CREATE/ALTER ... SECRET.
///  - A credential-like name (secret, token, pass, api_key, ...) followed by '=', ':',
///  TO,
///    a quote, '(', ')', ',' or a comment, which also masks column lists like
///    (id, password). A bare "key" counts only before '=', ':', TO or a quote.
///  - user:password@ in a URL, and md:<token>@db, also when the text ends before the @.
///  - source_code and config arguments of md_create_flight.
/// Over-masking is accepted (`WHERE key = 5` is masked), but a lone quoted name
/// ("api_key" not found) stays readable. If the patterns fail, the text becomes
/// "(redacted)".
std::string RedactSecretStatements(std::string_view query);

/// \brief Drop the statement echo DuckDB appends to its error messages.
///
/// MotherDuck puts it in the primary message itself, so libpq's terse verbosity does
/// not remove it: "Parser Error: ...\n\nLINE 1: SELECT ...\n       ^". Everything from
/// the first line that starts with "LINE <n>:" to the end is removed, along with the
/// blank lines before it. Text without such a line is returned unchanged.
std::string StripStatementEcho(std::string_view text);

/// \brief StripStatementEcho, then RedactSecretStatements.
///
/// The redactor the driver installs; it sees SQL as well as server text.
std::string RedactMotherDuckErrorText(std::string_view text);

/// \brief Install RedactMotherDuckErrorText as the driver's error-message redactor.
void InstallMotherDuckQueryRedactor();

}  // namespace adbcpq
