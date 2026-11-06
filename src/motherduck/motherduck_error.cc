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

// Defines ClassifyServerError, which the upstream driver's MakeStatus calls.

#include "motherduck_error.h"

#include <string_view>

#include "error.h"

namespace adbcpq {

namespace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view text, std::string_view part) {
  return text.find(part) != std::string_view::npos;
}

}  // namespace

bool MotherduckIsMissingObject(const char* primary_message) {
  if (primary_message == nullptr) return false;
  std::string_view message(primary_message);
  // Not a regclass cast's "Invalid Input Error: relation ... does not exist":
  // the endpoint also reports that for a quoted, qualified name of a table that
  // exists (#13), so it doesn't show that the table is missing.

  // Catalog "db" does not exist!
  if (StartsWith(message, "Binder Error: Catalog \"")) {
    return Contains(message, "\" does not exist");
  }
  // Scalar Function with name f does not exist!
  if (StartsWith(message, "Catalog Error: ")) {
    return Contains(message, " with name ") && Contains(message, " does not exist");
  }
  return false;
}

AdbcStatusCode ClassifyServerError(const char* sqlstate, const char* primary_message) {
  if (sqlstate && std::string_view(sqlstate) == "XXUUU" &&
      MotherduckIsMissingObject(primary_message)) {
    return ADBC_STATUS_NOT_FOUND;
  }
  return ClassifySqlState(sqlstate);
}

}  // namespace adbcpq
