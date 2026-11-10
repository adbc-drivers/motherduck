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
/// A few regexes over the raw text, ignoring quoting and comments, so they also see
/// inside literals, comments and $$ bodies. At the first match everything after the kept
/// part is dropped and " ..." is appended:
///  - CREATE/ALTER ... SECRET: kept up to SECRET.
///  - A name containing secret, token, pass, key or similar, followed by '=', ':', TO, a
///    single quote, '(', ')', ',' or a comment opener (or a double quote after a space):
///    kept up to the name. This also masks column lists such as (id, password).
///  - user:password@ in a URL, and md:<token>@db.
///  - source_code and config arguments of md_create_flight.
/// This favours masking too much over leaking (`WHERE key = 5` is masked). A quoted name
/// on its own ("api_key" not found) is left readable. Other text is returned unchanged.
/// If the patterns can't be built or the matcher fails, the whole text becomes
/// "(redacted)".
std::string RedactSecretStatements(std::string_view query);

/// \brief Install RedactSecretStatements as the driver's error-message query redactor.
void InstallMotherDuckQueryRedactor();

}  // namespace adbcpq
