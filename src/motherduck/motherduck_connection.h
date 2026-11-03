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

#include <cstddef>
#include <cstdint>
#include <vector>

#include <arrow-adbc/adbc.h>

#include "connection.h"

namespace adbcpq {

/// \brief The info codes GetInfo answers for MotherDuck, from those requested
/// (all supported codes if info_codes is null).
///
/// Drops ADBC_INFO_VENDOR_VERSION: the PostgreSQL driver reports the server's
/// server_version_num, which isn't a MotherDuck version.
std::vector<uint32_t> MotherduckInfoCodes(const uint32_t* info_codes,
                                          size_t info_codes_length);

class MotherduckConnection : public PostgresConnection {
 public:
  AdbcStatusCode GetInfo(struct AdbcConnection* connection, const uint32_t* info_codes,
                         size_t info_codes_length, struct ArrowArrayStream* out,
                         struct AdbcError* error) override;
};

}  // namespace adbcpq
