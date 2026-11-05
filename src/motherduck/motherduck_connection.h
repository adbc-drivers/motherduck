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

/// \brief Whether an error message is the endpoint's for a missing relation.
///
/// The endpoint reports it as an Invalid Input Error with SQLSTATE XXUUU
/// rather than PostgreSQL's 42P01, so the base driver can't map it to
/// ADBC_STATUS_NOT_FOUND.
bool MotherduckIsMissingRelation(const char* message);

/// \brief The GetObjects queries MotherDuck replaces.
///
/// They replace the PostgreSQL driver's queries that fail on the endpoint:
/// - Schemas come from duckdb_schemas() for the current database, since
///   pg_namespace also lists the temp database's main schema.
/// - Tables cast their parameters, which the endpoint can't infer types for,
///   and match table types without casting to an array.
/// - Constraints come from duckdb_constraints(), since the pg_constraint query
///   exceeds the endpoint's memory limit.
///
/// The schema and table filters ignore case, as DuckDB identifiers do.
PostgresGetObjectsQueryOverrides MotherduckGetObjectsQueryOverrides();

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
  /// Fails with ADBC_STATUS_NOT_FOUND for a missing table. The lookup is the
  /// PostgreSQL driver's regclass cast, so names resolve as they do in a query
  /// (ignoring case, through the search path); only its error is remapped. As
  /// on PostgreSQL, the failed lookup aborts an open transaction.
  AdbcStatusCode GetTableSchema(const char* catalog, const char* db_schema,
                                const char* table_name, struct ArrowSchema* schema,
                                struct AdbcError* error) override;

 protected:
  PostgresGetObjectsQueryOverrides GetObjectsQueryOverrides() const override;
};

}  // namespace adbcpq
