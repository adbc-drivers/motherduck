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

#include "driver/framework/status.h"
#include "postgres_type.h"

namespace adbcpq {

// MotherDuck's type OIDs are fixed, so a built-in table replaces querying pg_type:
// PostgreSQL's built-in types plus DuckDB-only types.
adbc::driver::Status InitializeMotherduckTypeResolver(PostgresTypeResolver& resolver);

// The built-in table, built once and shared. Do not modify it.
adbc::driver::Status SharedMotherduckTypeResolver(
    std::shared_ptr<PostgresTypeResolver>* out);

}  // namespace adbcpq
