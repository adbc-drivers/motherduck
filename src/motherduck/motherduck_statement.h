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

#include <libpq-fe.h>

#include "postgres_type.h"
#include "result_reader.h"
#include "statement.h"

namespace adbcpq {

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

 private:
  std::string query_;
};

class MotherduckStatement : public PostgresStatement {
 protected:
  std::unique_ptr<PqResultArrayReader> MakeResultReader(
      PGconn* conn, std::shared_ptr<PostgresTypeResolver> type_resolver,
      std::string query) override;
  bool IsCommandOk(PGresult* result) override;
};

}  // namespace adbcpq
