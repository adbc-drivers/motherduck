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

// Split a query into statements. Generic libpq SQL, not a MotherDuck rule.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <libpq-fe.h>

namespace adbcpq {

// Split a query on the semicolons between its statements, skipping those inside
// string literals, quoted identifiers, dollar quotes and comments. Statements
// with nothing but whitespace or comments are dropped. backslash_escapes says
// whether backslashes escape quotes in every literal, as they do when
// standard_conforming_strings is off, rather than only in E'' literals.
std::vector<std::string> SplitStatements(std::string_view query,
                                         bool backslash_escapes = false);

// Whether every string literal on this connection takes backslash escapes
// (standard_conforming_strings is off).
bool BackslashEscapes(PGconn* conn);

}  // namespace adbcpq
