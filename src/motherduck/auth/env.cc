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

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "auth/env.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace adbcpq::auth {

#ifdef _WIN32
std::optional<std::string> GetEnvUtf8(const char* name) {
  // Names are ASCII.
  const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
  std::wstring value;
  DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  while (size > 0) {
    value.resize(size);
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    // Less than the buffer: the value and its terminator fit. More: it grew
    // meanwhile, so retry with the new size.
    if (written < size) {
      value.resize(written);
      break;
    }
    size = written;
  }
  if (value.empty()) return std::nullopt;
  const int length =
      WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                          nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::nullopt;
  std::string utf8(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      utf8.data(), length, nullptr, nullptr);
  return utf8;
}
#else
std::optional<std::string> GetEnvUtf8(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}
#endif

}  // namespace adbcpq::auth
