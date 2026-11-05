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

#include <memory>

#include <libpq-fe.h>

#include "driver/framework/objects.h"

namespace adbcpq {

/// \brief A GetObjects helper that reads DuckDB's metadata functions
/// (duckdb_databases(), duckdb_schemas(), duckdb_tables(), duckdb_views(),
/// duckdb_columns() and duckdb_constraints()).
///
/// It lists every attached database, not only the current one. It loads each
/// level with one query up front rather than one per table. Every filter is an
/// ILIKE pattern, since DuckDB identifiers ignore case. A column filter keeps
/// the constraints on a matching column.
std::unique_ptr<adbc::driver::GetObjectsHelper> MakeMotherduckGetObjectsHelper(
    PGconn* conn);

}  // namespace adbcpq
