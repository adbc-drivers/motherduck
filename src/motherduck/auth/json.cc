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

#include "auth/json.h"

#include <cstdlib>
#include <limits>
#include <memory>

#include <yyjson.h>

namespace adbcpq::auth {

namespace {

// The value of a member of the document's root object, or null.
yyjson_val* Member(yyjson_doc* doc, const char* key) {
  return doc ? yyjson_obj_get(yyjson_doc_get_root(doc), key) : nullptr;
}

}  // namespace

void JsonObject::Deleter::operator()(yyjson_doc* doc) const { yyjson_doc_free(doc); }

JsonObject JsonObject::Parse(std::string_view text) {
  // Without flags, yyjson accepts only standard JSON and rejects trailing content.
  JsonObject result(yyjson_read(text.data(), text.size(), YYJSON_READ_NOFLAG));
  if (!result.valid() || !yyjson_is_obj(yyjson_doc_get_root(result.doc_.get()))) {
    return JsonObject(nullptr);
  }
  return result;
}

std::optional<std::string> JsonObject::GetString(const char* key) const {
  yyjson_val* value = Member(doc_.get(), key);
  if (!yyjson_is_str(value)) return std::nullopt;
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

std::optional<int64_t> JsonObject::GetInt(const char* key) const {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  yyjson_val* value = Member(doc_.get(), key);
  if (yyjson_is_uint(value)) {
    const uint64_t number = yyjson_get_uint(value);
    return number > static_cast<uint64_t>(kMax) ? kMax : static_cast<int64_t>(number);
  }
  if (yyjson_is_sint(value)) return yyjson_get_sint(value);
  if (yyjson_is_real(value)) {
    const double number = yyjson_get_real(value);
    if (number >= static_cast<double>(kMax)) return kMax;
    if (number <= static_cast<double>(kMin)) return kMin;
    return static_cast<int64_t>(number);
  }
  return std::nullopt;
}

std::optional<std::string> CredentialsJson(std::string_view token) {
  std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  if (!doc) return std::nullopt;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  yyjson_mut_doc_set_root(doc.get(), root);
  yyjson_mut_obj_add_int(doc.get(), root, "version", 1);
  yyjson_mut_obj_add_strncpy(doc.get(), root, "motherduck_token", token.data(),
                             token.size());
  // Two-space indentation and a final newline.
  size_t length = 0;
  std::unique_ptr<char, decltype(&std::free)> text(
      yyjson_mut_write(doc.get(), YYJSON_WRITE_PRETTY_TWO_SPACES, &length), std::free);
  if (!text) return std::nullopt;
  std::string result(text.get(), length);
  result += '\n';
  return result;
}

}  // namespace adbcpq::auth
