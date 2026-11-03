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

#include "motherduck_statement.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <nanoarrow/nanoarrow.h>

#include "error.h"
#include "motherduck_pipeline.h"

namespace adbcpq {

Status MotherduckResultReader::ExecuteAll(int64_t* affected_rows) {
  // For the case where we don't need a result, we either need to exhaust the bind
  // stream (if there is one) or execute the query without binding.
  if (bind_stream_) {
    UNWRAP_STATUS(bind_stream_->Begin([] { return Status::Ok(); }));

    const bool has_na_params = std::any_of(
        bind_stream_->bind_schema_fields.begin(), bind_stream_->bind_schema_fields.end(),
        [](const ArrowSchemaView& view) { return view.type == NANOARROW_TYPE_NA; });
    if (has_na_params) {
      // Prepare without parameter types so PostgreSQL can infer them. This is required
      // to resolve Arrow NA (all-null) parameters to the expected PostgreSQL types.
      UNWRAP_STATUS(helper_.Prepare());
      UNWRAP_STATUS(helper_.DescribePrepared());
      PostgresType expected_types;
      UNWRAP_STATUS(helper_.ResolveParamTypes(*type_resolver_, &expected_types));
      UNWRAP_STATUS(bind_stream_->ReconcileWithExpectedTypes(expected_types));
    }

    // SetParamTypes() may begin a transaction, so from here on errors abort.
    UNWRAP_STATUS(AbortBindOnError(
        bind_stream_->SetParamTypes(conn_, *type_resolver_, autocommit_)));
    UNWRAP_STATUS(AbortBindOnError(helper_.Prepare(bind_stream_->param_types)));

    // Send every row through one pipeline instead of waiting on each in turn.
    // The rows share one sync, so they run in one implicit transaction.
    int64_t total_rows = 0;
    bool rows_unknown = false;  // some row's command reported no count
    Status status;
    Status pipeline_status = RunPipeline(
        conn_,
        [&](bool* sent) -> Status {
          UNWRAP_STATUS(bind_stream_->EnsureNextRow());
          *sent = bind_stream_->current->release != nullptr;
          if (!*sent) return Status::Ok();
          return SendCurrentRow(bind_stream_.get(), conn_, kPgBinaryFormat);
        },
        [&](size_t, PGresult* result) {
          const ExecStatusType result_status = PQresultStatus(result);
          if (result_status == PGRES_COMMAND_OK || result_status == PGRES_TUPLES_OK) {
            helper_.SetResult(result);
            const int64_t rows = helper_.AffectedRows();
            if (rows < 0) {
              rows_unknown = true;
            } else {
              total_rows += rows;
            }
          } else if (result_status != PGRES_PIPELINE_ABORTED && status.ok()) {
            status =
                MakeStatus(result, "[libpq] Failed to execute prepared statement: {} {}",
                           PQresStatus(result_status), PQresultErrorMessage(result));
            PQclear(result);
          } else {
            PQclear(result);
          }
        });

    // A row's own error explains more than the pipeline failing after it.
    if (!status.ok() || !pipeline_status.ok()) {
      bind_stream_->Abort(conn_);
      bind_stream_.reset();
      if (!status.ok()) return status;
      return pipeline_status;
    }

    if (affected_rows) {
      *affected_rows = rows_unknown ? -1 : total_rows;
    }
    Status cleanup_status = bind_stream_->Cleanup(conn_);
    if (!cleanup_status.ok()) bind_stream_->Abort(conn_);
    bind_stream_.reset();
    UNWRAP_STATUS(cleanup_status);
  } else {
    UNWRAP_STATUS(ExecuteScript(conn_, query_, &helper_));

    if (affected_rows != nullptr) {
      *affected_rows = helper_.AffectedRows();
    }
  }

  return Status::Ok();
}

std::unique_ptr<PqResultArrayReader> MotherduckStatement::MakeResultReader(
    PGconn* conn, std::shared_ptr<PostgresTypeResolver> type_resolver,
    std::string query) {
  return std::make_unique<MotherduckResultReader>(conn, std::move(type_resolver),
                                                  std::move(query));
}

// MotherDuck answers DDL with an empty result set instead of a command tag.
bool MotherduckStatement::IsCommandOk(PGresult* result) { return IsResultOk(result); }

}  // namespace adbcpq
