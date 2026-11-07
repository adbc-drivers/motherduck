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
#include <memory>
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
  /// Fails with ADBC_STATUS_NOT_IMPLEMENTED: MotherDuck has no pg_stats.
  AdbcStatusCode GetStatistics(const char* catalog, const char* db_schema,
                               const char* table_name, bool approximate,
                               struct ArrowArrayStream* out,
                               struct AdbcError* error) override;
  /// Fails with ADBC_STATUS_NOT_IMPLEMENTED, like GetStatistics.
  AdbcStatusCode GetStatisticNames(struct ArrowArrayStream* out,
                                   struct AdbcError* error) override;
  /// Describes SELECT * FROM the table, so the name resolves as in a query:
  /// ignoring case, through the search path, and in the given catalog. A
  /// catalog without a schema means the current schema in the current
  /// database and main in any other. A missing table fails with
  /// ADBC_STATUS_NOT_FOUND.
  AdbcStatusCode GetTableSchema(const char* catalog, const char* db_schema,
                                const char* table_name, struct ArrowSchema* schema,
                                struct AdbcError* error) override;
  /// The table types MotherDuck's GetObjects reports: table and view.
  AdbcStatusCode GetTableTypes(struct AdbcConnection* connection,
                               struct ArrowArrayStream* out,
                               struct AdbcError* error) override;

 protected:
  std::unique_ptr<adbc::driver::GetObjectsHelper> MakeGetObjectsHelper() override;
};

}  // namespace adbcpq
