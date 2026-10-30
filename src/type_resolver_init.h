// Copyright (c) 2026 ADBC Drivers Contributors
//
// This file has been modified from its original version, which is
// under the Apache License:
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "driver/framework/status.h"
#include "postgres_type.h"

namespace adbcpq {

// MotherDuck reports the standard PostgreSQL OIDs for the types it shares with
// PostgreSQL, so the built-in table resolves them without querying pg_type (which
// MotherDuck only partially populates). DuckDB-only types use OIDs outside the
// table and come back as opaque binary.
adbc::driver::Status InitializeTypeResolver(PostgresTypeResolver& resolver);

}  // namespace adbcpq

// exposed for testing

ADBC_EXPORT
adbc::driver::Status InternalAdbcInitializeTypeResolver(
    adbcpq::PostgresTypeResolver& resolver);
