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

#include <optional>
#include <string>

namespace adbcpq::auth {

/// \brief An environment variable as UTF-8, or nullopt if it's unset or empty.
///
/// On Windows this reads the process environment rather than this DLL's C
/// runtime copy, which misses changes the application makes after the DLL loads.
std::optional<std::string> GetEnvUtf8(const char* name);

}  // namespace adbcpq::auth
