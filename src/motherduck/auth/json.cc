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

std::optional<std::string> UpdateCredentialsJson(std::string_view existing,
                                                 std::optional<std::string_view> token) {
  std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(
      nullptr, yyjson_mut_doc_free);
  if (!existing.empty()) {
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> parsed(
        yyjson_read(existing.data(), existing.size(), YYJSON_READ_NOFLAG),
        yyjson_doc_free);
    yyjson_val* root = parsed ? yyjson_doc_get_root(parsed.get()) : nullptr;
    if (yyjson_is_obj(root)) {
      yyjson_val* version = yyjson_obj_get(root, "version");
      if (version && !(yyjson_is_int(version) && yyjson_get_int(version) == 1)) {
        return std::nullopt;
      }
      doc.reset(yyjson_doc_mut_copy(parsed.get(), nullptr));
      if (!doc) return std::nullopt;
    }
  }
  if (!doc) {
    doc.reset(yyjson_mut_doc_new(nullptr));
    if (!doc) return std::nullopt;
    yyjson_mut_doc_set_root(doc.get(), yyjson_mut_obj(doc.get()));
  }
  yyjson_mut_val* root = yyjson_mut_doc_get_root(doc.get());
  if (!yyjson_mut_obj_get(root, "version")) {
    yyjson_mut_obj_insert(root, yyjson_mut_str(doc.get(), "version"),
                          yyjson_mut_int(doc.get(), 1), 0);
  }
  if (token) {
    yyjson_mut_val* key = yyjson_mut_str(doc.get(), "motherduck_token");
    yyjson_mut_val* value = yyjson_mut_strncpy(doc.get(), token->data(), token->size());
    // In place if it's there, else after the version, as other tools write it.
    if (!yyjson_mut_obj_replace(root, key, value)) {
      yyjson_mut_obj_insert(root, key, value, 1);
    }
  } else {
    yyjson_mut_obj_remove_str(root, "motherduck_token");
    if (yyjson_mut_obj_size(root) == 1) return std::string();
  }
  // Like JSON.stringify(credentials, null, 2), which other tools use.
  size_t length = 0;
  std::unique_ptr<char, decltype(&std::free)> text(
      yyjson_mut_write(doc.get(), YYJSON_WRITE_PRETTY_TWO_SPACES, &length), std::free);
  if (!text) return std::nullopt;
  std::string result(text.get(), length);
  result += '\n';
  return result;
}

}  // namespace adbcpq::auth
