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

// The objects postgresql.cc creates for each ADBC handle: the MotherDuck
// subclasses of the upstream driver's classes.

#pragma once

#include <memory>

#include "connection.h"
#include "database.h"
#include "statement.h"

namespace adbcpq {

std::shared_ptr<PostgresDatabase> MakeDatabase();
std::shared_ptr<PostgresConnection> MakeConnection();
std::shared_ptr<PostgresStatement> MakeStatement();

}  // namespace adbcpq
