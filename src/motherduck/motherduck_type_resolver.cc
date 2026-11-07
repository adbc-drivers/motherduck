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

#include "motherduck_type_resolver.h"

#include <array>
#include <memory>
#include <string>
#include <utility>

#include "type_resolver_init.h"

using adbc::driver::Status;

namespace adbcpq {

namespace {

// DuckDB-only types, which MotherDuck sends as text under fixed OIDs. Elements
// must precede their arrays.
constexpr std::array<PostgresTypeResolver::Item, 19> kMotherDuckTypeItems = {{
    {17000, "duckdb_nested_list", "textrecv", 0, 0, 0},
    {17001, "duckdb_struct", "textrecv", 0, 0, 0},
    {17002, "_duckdb_struct", "array_recv", 17001, 0, 0},
    {17003, "duckdb_map", "textrecv", 0, 0, 0},
    {17004, "_duckdb_map", "array_recv", 17003, 0, 0},
    {17005, "duckdb_enum", "textrecv", 0, 0, 0},
    {17006, "_duckdb_enum", "array_recv", 17005, 0, 0},
    {17007, "duckdb_union", "textrecv", 0, 0, 0},
    {17008, "_duckdb_union", "array_recv", 17007, 0, 0},
    {17009, "duckdb_timestamp_ns", "textrecv", 0, 0, 0},
    {17010, "_duckdb_timestamp_ns", "array_recv", 17009, 0, 0},
    {17011, "duckdb_variant", "textrecv", 0, 0, 0},
    {17012, "_duckdb_variant", "array_recv", 17011, 0, 0},
    {17013, "duckdb_time_ns", "textrecv", 0, 0, 0},
    {17014, "_duckdb_time_ns", "array_recv", 17013, 0, 0},
    {17015, "duckdb_geometry", "textrecv", 0, 0, 0},
    {17016, "_duckdb_geometry", "array_recv", 17015, 0, 0},
    {17017, "duckdb_type", "textrecv", 0, 0, 0},
    {17018, "_duckdb_type", "array_recv", 17017, 0, 0},
}};

}  // namespace

Status InitializeMotherduckTypeResolver(PostgresTypeResolver& resolver) {
  // PostgreSQL types first, so GetOID() keeps returning the standard OIDs
  UNWRAP_STATUS(InitializeTypeResolver(resolver));
  ArrowError na_error = {};
  for (const auto& item : kMotherDuckTypeItems) {
    UNWRAP_NANOARROW(na_error, Internal, resolver.Insert(item, &na_error));
  }
  return Status::Ok();
}

Status SharedMotherduckTypeResolver(std::shared_ptr<PostgresTypeResolver>* out) {
  static const auto kShared =
      []() -> std::pair<std::shared_ptr<PostgresTypeResolver>, std::string> {
    auto resolver = std::make_shared<PostgresTypeResolver>();
    Status status = InitializeMotherduckTypeResolver(*resolver);
    if (!status.ok()) return {nullptr, status.message()};
    return {std::move(resolver), ""};
  }();
  if (!kShared.first) {
    return Status::Internal("Failed to build the type table: ", kShared.second);
  }
  *out = kShared.first;
  return Status::Ok();
}

}  // namespace adbcpq
