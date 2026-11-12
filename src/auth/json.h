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

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct yyjson_doc;

namespace adbcpq::auth {

/// \brief A parsed JSON object, for reading the members of a response.
class JsonObject {
 public:
  /// \brief Parse standard JSON; the result is invalid unless the text is one
  ///   object.
  static JsonObject Parse(std::string_view text);

  bool valid() const { return doc_ != nullptr; }

  /// \brief A string member, or nullopt if it's missing or not a string.
  std::optional<std::string> GetString(const char* key) const;

  /// \brief The integer part of a number member, or nullopt if it's missing or
  ///   not a number.
  std::optional<int64_t> GetInt(const char* key) const;

 private:
  struct Deleter {
    void operator()(yyjson_doc* doc) const;
  };

  explicit JsonObject(yyjson_doc* doc) : doc_(doc) {}

  std::unique_ptr<yyjson_doc, Deleter> doc_;
};

/// \brief The contents of the credentials file the MotherDuck CLI writes, or
///   nullopt if the token isn't valid UTF-8.
std::optional<std::string> CredentialsJson(std::string_view token);

}  // namespace adbcpq::auth
