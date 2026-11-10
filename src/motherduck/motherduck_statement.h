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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <libpq-fe.h>

#include "postgres_type.h"
#include "result_reader.h"
#include "statement.h"

namespace adbcpq {

// The type to cast each bound parameter to, or "" for none. An all-null
// parameter has no type of its own, so it takes the type in null_param_oids, or
// none if that's empty.
Status ParameterCastTypes(const PostgresTypeResolver& type_resolver,
                          const struct ArrowSchema& bind_schema,
                          const std::vector<Oid>& null_param_oids,
                          std::vector<std::string>* types);

// Runs queries without a result through a libpq pipeline: bound rows are sent
// without waiting on each in turn, and multi-statement queries run as
// separate commands.
class MotherduckResultReader : public PqResultArrayReader {
 public:
  MotherduckResultReader(PGconn* conn,
                         std::shared_ptr<PostgresTypeResolver> type_resolver,
                         std::string query)
      : PqResultArrayReader(conn, std::move(type_resolver), query),
        query_(std::move(query)) {}

 protected:
  Status ExecuteAll(int64_t* affected_rows) override;
  Status PrepareWithParamTypes() override;
  Status ResolveNullParamTypes() override;
  std::string ResultColumnName(std::string name) const override;

 private:
  std::string query_;
  // Whether the query only prepares with its parameters cast
  bool needs_param_casts_ = false;
  // The casts the query was prepared with, if any
  std::vector<std::string> param_casts_;
};

class MotherduckStatement : public PostgresStatement {
 protected:
  std::unique_ptr<PqResultArrayReader> MakeResultReader(
      PGconn* conn, std::shared_ptr<PostgresTypeResolver> type_resolver,
      std::string query) override;
  bool IsCommandOk(PGresult* result) override;
  Status PrepareQuery(PGconn* conn, const std::string& query,
                      const std::vector<Oid>& param_oids,
                      const struct ArrowSchema* bind_schema) override;
  std::string ResultColumnName(std::string name) const override;
  bool SameColumnName(std::string_view table_column,
                      std::string_view source_column) const override;

 private:
  // The casts the last query was prepared with, if any
  std::vector<std::string> param_casts_;
};

}  // namespace adbcpq
