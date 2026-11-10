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
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <libpq-fe.h>
#include <nanoarrow/nanoarrow.hpp>

#include "driver/common/utils.h"
#include "driver/framework/objects.h"
#include "driver/framework/status.h"
#include "driver/framework/utility.h"
#include "motherduck_get_objects.h"
#include "postgres_type.h"
#include "result_helper.h"

namespace adbcpq {

namespace {

constexpr uint32_t kMotherduckInfoCodes[] = {
    ADBC_INFO_VENDOR_NAME,         ADBC_INFO_DRIVER_NAME,
    ADBC_INFO_DRIVER_VERSION,      ADBC_INFO_DRIVER_ARROW_VERSION,
    ADBC_INFO_DRIVER_ADBC_VERSION,
};

std::string Text(const PqRecord& record) { return std::string(record.data, record.len); }

constexpr char kNoStatistics[] =
    "[motherduck] %s is not supported by the MotherDuck driver: MotherDuck has no "
    "pg_stats to read statistics from";

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
  if (table_name == nullptr) {
    InternalAdbcSetError(error, "[motherduck] GetTableSchema requires a table name");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // A catalog without a schema means the session's schema in the current
  // database, as an unqualified name does, and main in any other. The current
  // database may be named as itself or as the connection's current catalog,
  // which is md: when connected to md: (#14).
  std::string resolved_catalog;
  std::string default_schema;
  if (catalog != nullptr && db_schema == nullptr) {
    const char* current_catalog = PQdb(conn());
    PqResultHelper helper(
        conn(),
        "SELECT CASE WHEN current THEN current_database() ELSE $1::VARCHAR END, "
        "CASE WHEN current THEN coalesce(current_schema(), 'main') ELSE 'main' END "
        "FROM (SELECT lower(current_database()) = lower($1::VARCHAR) "
        "OR $1::VARCHAR = $2::VARCHAR AS current)");
    RAISE_STATUS(error,
                 helper.Execute({catalog, current_catalog ? current_catalog : ""}));
    if (helper.NumRows() != 1) {
      InternalAdbcSetError(error, "[motherduck] Failed to look up the default schema");
      return ADBC_STATUS_INTERNAL;
    }
    resolved_catalog = Text(helper.Row(0)[0]);
    default_schema = Text(helper.Row(0)[1]);
    catalog = resolved_catalog.c_str();
    db_schema = default_schema.c_str();
  }

  std::string name;
  for (const char* part : {catalog, db_schema, table_name}) {
    if (part == nullptr) continue;
    char* quoted = PQescapeIdentifier(conn(), part, std::strlen(part));
    if (quoted == nullptr) {
      InternalAdbcSetError(error, "[motherduck] Failed to quote '%s': %s", part,
                           PQerrorMessage(conn()));
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (!name.empty()) name += ".";
    name += quoted;
    PQfreemem(quoted);
  }

  // Describing a query resolves the name as any query does: ignoring case and
  // through the search path. A missing table fails with 42P01 (NOT_FOUND).
  PqResultHelper helper(conn(), "SELECT * FROM " + name);
  RAISE_STATUS(error, helper.Prepare());
  RAISE_STATUS(error, helper.DescribePrepared());
  PostgresType output_type;
  RAISE_STATUS(error, helper.ResolveOutputTypes(*type_resolver(), &output_type));

  nanoarrow::UniqueSchema result;
  ArrowSchemaInit(result.get());
  CHECK_NA(INTERNAL, output_type.SetSchema(result.get(), std::string(VendorName())),
           error);
  result.move(schema);
  return ADBC_STATUS_OK;
}

AdbcStatusCode MotherduckConnection::GetTableTypes(struct AdbcConnection* connection,
                                                   struct ArrowArrayStream* out,
                                                   struct AdbcError* error) {
  RAISE_STATUS(error, adbc::driver::MakeTableTypesStream({"table", "view"}, out));
  return ADBC_STATUS_OK;
}

std::unique_ptr<adbc::driver::GetObjectsHelper>
MotherduckConnection::MakeGetObjectsHelper() {
  return MakeMotherduckGetObjectsHelper(conn());
}

}  // namespace adbcpq
