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
/// A crude scan of the raw text, ignoring quoting and comments, so it also sees inside
/// literals, comments and $$ bodies. At the first trigger everything after it is
/// dropped:
///  - CREATE/ALTER ... SECRET: from the first '(' on becomes "(...)".
///  - A name with a secret, token, password or key part followed by '=', ':', TO or a
///    string (name = v, name TO v, PASSWORD 'v'): the value and the rest are dropped.
///  - user:password@ in a URL: the password and the rest are dropped.
/// This favours masking too much over leaking. Other text is returned unchanged.
std::string RedactSecretStatements(std::string_view query);

/// \brief Install RedactSecretStatements as the driver's error-message query redactor.
void InstallMotherDuckQueryRedactor();

}  // namespace adbcpq
