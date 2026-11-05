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
#include <string>
#include <vector>

#include "driver/common/utils.h"
#include "driver/framework/status.h"
#include "result_helper.h"

namespace adbcpq {

namespace {

constexpr uint32_t kMotherduckInfoCodes[] = {
    ADBC_INFO_VENDOR_NAME,         ADBC_INFO_DRIVER_NAME,
    ADBC_INFO_DRIVER_VERSION,      ADBC_INFO_DRIVER_ARROW_VERSION,
    ADBC_INFO_DRIVER_ADBC_VERSION,
};

constexpr char kSchemasQuery[] =
    "SELECT schema_name FROM duckdb_schemas() "
    "WHERE database_name = current_database() "
    "AND schema_name NOT IN ('information_schema', 'pg_catalog')";

// Parameterized on schema_name, relkind. relkind is a PostgreSQL array literal
// such as {"r", "v"}, which DuckDB can't cast to VARCHAR[], so this matches the
// quoted relkind in the text. The temp database also has a main schema, so only
// the current database's schemas match.
constexpr char kTablesQuery[] =
    "SELECT c.relname, CASE c.relkind WHEN 'r' THEN 'table' WHEN 'v' THEN 'view' "
    "WHEN 'm' THEN 'materialized view' WHEN 't' THEN 'TOAST table' "
    "WHEN 'f' THEN 'foreign table' WHEN 'p' THEN 'partitioned table' END "
    "AS reltype FROM pg_catalog.pg_class c "
    "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "WHERE n.nspname = $1::VARCHAR "
    "AND contains($2::VARCHAR, '\"' || c.relkind || '\"') "
    "AND n.oid IN (SELECT oid FROM duckdb_schemas() "
    "WHERE database_name = current_database())";

// Parameterized on schema_name, table_name. DuckDB foreign keys can only
// reference tables in the same schema.
constexpr char kConstraintsQuery[] =
    "WITH cons AS ( "
    "    SELECT constraint_name AS conname, constraint_type AS contype, "
    "    constraint_column_names AS colnames, "
    "    CASE WHEN constraint_type = 'FOREIGN KEY' THEN schema_name END AS fschema, "
    "    referenced_table AS ftable, referenced_column_names AS fcolnames "
    "    FROM duckdb_constraints() "
    "    WHERE database_name = current_database() AND schema_name = $1::VARCHAR "
    "    AND table_name = $2::VARCHAR "
    "    AND constraint_type IN ('CHECK', 'UNIQUE', 'PRIMARY KEY', 'FOREIGN KEY') "
    ") "
    "SELECT conname, contype, colnames, fschema, ftable, fcolnames FROM cons";

// Parameterized on db_schema (current_schema() if empty), table_name.
constexpr char kTableExistsQuery[] =
    "SELECT 1 FROM information_schema.tables "
    "WHERE table_catalog = current_database() "
    "AND table_schema = COALESCE(NULLIF($1::VARCHAR, ''), current_schema()) "
    "AND table_name = $2::VARCHAR";

constexpr char kNoStatistics[] =
    "[motherduck] %s is not supported by the MotherDuck driver: MotherDuck has no "
    "pg_stats to read statistics from";

}  // namespace

PostgresGetObjectsQueryOverrides MotherduckGetObjectsQueryOverrides() {
  PostgresGetObjectsQueryOverrides overrides;
  overrides.all_schemas = kSchemasQuery;
  overrides.some_schemas = std::string(kSchemasQuery) + " AND schema_name = $1::VARCHAR";
  overrides.all_tables = kTablesQuery;
  overrides.some_tables = std::string(kTablesQuery) + " AND c.relname LIKE $3::VARCHAR";
  overrides.all_constraints = kConstraintsQuery;
  overrides.some_constraints =
      std::string(kConstraintsQuery) + " WHERE conname LIKE $3::VARCHAR";
  return overrides;
}

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

AdbcStatusCode MotherduckConnection::GetStatistics(
    const char* catalog, const char* db_schema, const char* table_name, bool approximate,
    struct ArrowArrayStream* out, struct AdbcError* error) {
  InternalAdbcSetError(error, kNoStatistics, "AdbcConnectionGetStatistics");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode MotherduckConnection::GetStatisticNames(struct ArrowArrayStream* out,
                                                       struct AdbcError* error) {
  InternalAdbcSetError(error, kNoStatistics, "AdbcConnectionGetStatisticNames");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode MotherduckConnection::GetTableSchema(const char* catalog,
                                                    const char* db_schema,
                                                    const char* table_name,
                                                    struct ArrowSchema* schema,
                                                    struct AdbcError* error) {
  // Check first: a failed lookup would abort an open transaction.
  PqResultHelper exists(conn(), kTableExistsQuery);
  RAISE_STATUS(error, exists.Execute({db_schema ? db_schema : "", table_name}));
  if (exists.NumRows() == 0) {
    if (db_schema) {
      InternalAdbcSetError(error, "[motherduck] table \"%s\".\"%s\" does not exist",
                           db_schema, table_name);
    } else {
      InternalAdbcSetError(error, "[motherduck] table \"%s\" does not exist", table_name);
    }
    return ADBC_STATUS_NOT_FOUND;
  }
  return PostgresConnection::GetTableSchema(catalog, db_schema, table_name, schema,
                                            error);
}

PostgresGetObjectsQueryOverrides MotherduckConnection::GetObjectsQueryOverrides() const {
  return MotherduckGetObjectsQueryOverrides();
}

}  // namespace adbcpq
