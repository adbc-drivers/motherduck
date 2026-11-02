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

#include "motherduck_connection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace adbcpq {

namespace {

constexpr uint32_t kMotherduckInfoCodes[] = {
    ADBC_INFO_VENDOR_NAME,         ADBC_INFO_DRIVER_NAME,
    ADBC_INFO_DRIVER_VERSION,      ADBC_INFO_DRIVER_ARROW_VERSION,
    ADBC_INFO_DRIVER_ADBC_VERSION,
};

}  // namespace

std::vector<uint32_t> MotherduckInfoCodes(const uint32_t* info_codes,
                                          size_t info_codes_length) {
  if (info_codes == nullptr) {
    return {std::begin(kMotherduckInfoCodes), std::end(kMotherduckInfoCodes)};
  }
  std::vector<uint32_t> codes;
  std::copy_if(info_codes, info_codes + info_codes_length, std::back_inserter(codes),
               [](uint32_t code) { return code != ADBC_INFO_VENDOR_VERSION; });
  return codes;
}

AdbcStatusCode MotherduckConnection::GetInfo(struct AdbcConnection* connection,
                                             const uint32_t* info_codes,
                                             size_t info_codes_length,
                                             struct ArrowArrayStream* out,
                                             struct AdbcError* error) {
  std::vector<uint32_t> codes = MotherduckInfoCodes(info_codes, info_codes_length);
  // An empty vector's data() may be null, which the base takes as "all codes".
  const uint32_t* data = codes.empty() ? info_codes : codes.data();
  return PostgresConnection::GetInfo(connection, data, codes.size(), out, error);
}

}  // namespace adbcpq
