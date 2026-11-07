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

namespace {

// The endpoint can't prepare a query whose result column types depend on
// untyped parameters (e.g. SELECT $1), and ignores the parameter types given
// with the prepare (#17).
bool IsAmbiguousParameterError(const Status& status) {
  return status.message().find("ambiguous result column types") != std::string::npos;
}

// The type to cast each bound parameter to, or an empty string to leave it
// uncast.
Status ParameterCastTypes(const PostgresTypeResolver& type_resolver,
                          const struct ArrowSchema& bind_schema,
                          std::vector<std::string>* types) {
  struct ArrowError na_error;
  ArrowErrorInit(&na_error);
  types->assign(bind_schema.n_children, "");
  for (int64_t i = 0; i < bind_schema.n_children; i++) {
    struct ArrowSchemaView view;
    UNWRAP_NANOARROW(na_error, Internal,
                     ArrowSchemaViewInit(&view, bind_schema.children[i], &na_error));
    // An all-null parameter has no type of its own.
    if (view.type == NANOARROW_TYPE_NA) continue;

    PostgresType type;
    UNWRAP_NANOARROW(na_error, Internal,
                     PostgresType::FromSchema(type_resolver, bind_schema.children[i],
                                              &type, &na_error));
    if (type.type_id() == PostgresTypeId::kNumeric) {
      // A bare NUMERIC is DECIMAL(18,3) in DuckDB, so keep the decimal's own
      // precision and scale, and leave any DuckDB can't hold uncast.
      if (view.decimal_precision < 1 || view.decimal_precision > 38 ||
          view.decimal_scale < 0 || view.decimal_scale > view.decimal_precision) {
        continue;
      }
      (*types)[i] = "NUMERIC(" + std::to_string(view.decimal_precision) + ", " +
                    std::to_string(view.decimal_scale) + ")";
    } else {
      (*types)[i] = type.sql_type_name();
    }
  }
  return Status::Ok();
}

// Prepare query with each parameter cast to the type of its bound value, which
// fixes the types of any result columns that depend on it.
Status PrepareWithParamCasts(PGconn* conn, const std::string& query,
                             const std::vector<Oid>& param_oids,
                             const struct ArrowSchema& bind_schema,
                             const PostgresTypeResolver& type_resolver) {
  std::vector<std::string> types;
  UNWRAP_STATUS(ParameterCastTypes(type_resolver, bind_schema, &types));
  return PqResultHelper(conn, CastParameters(query, types, BackslashEscapes(conn)))
      .Prepare(param_oids);
}

}  // namespace

Status MotherduckResultReader::PrepareWithParamTypes() {
  Status status = PqResultArrayReader::PrepareWithParamTypes();
  if (status.ok() || !IsAmbiguousParameterError(status)) return status;
  return PrepareWithParamCasts(conn_, query_, bind_stream_->param_types,
                               bind_stream_->bind_schema.value, *type_resolver_);
}

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

    UNWRAP_STATUS(bind_stream_->SetParamTypes(conn_, *type_resolver_, autocommit_));
    UNWRAP_STATUS(PrepareWithParamTypes());

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
    UNWRAP_STATUS(bind_stream_->Cleanup(conn_));
    bind_stream_.reset();
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

Status MotherduckStatement::PrepareQuery(PGconn* conn, const std::string& query,
                                         const std::vector<Oid>& param_oids,
                                         const struct ArrowSchema* bind_schema) {
  Status status = PostgresStatement::PrepareQuery(conn, query, param_oids, bind_schema);
  if (status.ok() || !IsAmbiguousParameterError(status)) return status;
  if (bind_schema == nullptr) {
    return Status::NotImplemented(
        "[libpq] MotherDuck can't infer the types of this query's parameters; bind "
        "parameters first, or cast them in the query (e.g. $1::INTEGER): ",
        status.message());
  }
  return PrepareWithParamCasts(conn, query, param_oids, *bind_schema, type_resolver());
}

// MotherDuck answers DDL with an empty result set instead of a command tag.
bool MotherduckStatement::IsCommandOk(PGresult* result) { return IsResultOk(result); }

}  // namespace adbcpq
