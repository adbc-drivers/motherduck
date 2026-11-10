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
#include <string_view>
#include <utility>

#include <nanoarrow/nanoarrow.h>

#include "error.h"
#include "motherduck_pipeline.h"

namespace adbcpq {

namespace {

// The endpoint can't prepare a query like SELECT $1, whose result types depend
// on its parameters, and ignores parameter types sent with the prepare (#17).
bool IsAmbiguousParameterError(const Status& status) {
  // Only the first line, the server's error: later ones echo the query, which
  // may contain the phrase itself.
  const std::string message = status.message();
  return std::string_view(message)
             .substr(0, message.find('\n'))
             .find("ambiguous result column types") != std::string_view::npos;
}

// The type to cast a parameter of this type and Arrow schema to, or "" for none.
std::string CastTypeName(const PostgresType& type, const struct ArrowSchema* schema) {
  struct ArrowSchemaView view;
  if (ArrowSchemaViewInit(&view, schema, /*error=*/nullptr) != NANOARROW_OK) return "";
  if (view.type == NANOARROW_TYPE_DICTIONARY) {
    return CastTypeName(type, schema->dictionary);
  }
  // An all-null value has no precision or element type to keep.
  const bool all_null = view.type == NANOARROW_TYPE_NA;

  switch (type.type_id()) {
    case PostgresTypeId::kNumeric:
      if (all_null) return type.sql_type_name();
      // A bare NUMERIC is DECIMAL(18,3) in DuckDB, so keep precision and scale;
      // skip decimals DuckDB can't hold.
      if (view.decimal_precision < 1 || view.decimal_precision > 38 ||
          view.decimal_scale < 0 || view.decimal_scale > view.decimal_precision) {
        return "";
      }
      return "NUMERIC(" + std::to_string(view.decimal_precision) + ", " +
             std::to_string(view.decimal_scale) + ")";
    case PostgresTypeId::kArray: {
      if (type.n_children() != 1 || (!all_null && schema->n_children != 1)) return "";
      std::string element =
          CastTypeName(type.child(0), all_null ? schema : schema->children[0]);
      return element.empty() ? "" : element + "[]";
    }
    default:
      return type.sql_type_name();
  }
}

}  // namespace

Status ParameterCastTypes(const PostgresTypeResolver& type_resolver,
                          const struct ArrowSchema& bind_schema,
                          const std::vector<Oid>& null_param_oids,
                          std::vector<std::string>* types) {
  types->assign(bind_schema.n_children, "");
  for (int64_t i = 0; i < bind_schema.n_children; i++) {
    struct ArrowSchemaView view;
    if (ArrowSchemaViewInit(&view, bind_schema.children[i], /*error=*/nullptr) !=
        NANOARROW_OK) {
      continue;
    }
    // Leave a parameter uncast if its type can't be mapped, so the prepare
    // fails with the server's error rather than the driver's.
    PostgresType type;
    if (view.type == NANOARROW_TYPE_NA) {
      if (static_cast<size_t>(i) >= null_param_oids.size() ||
          type_resolver.Find(null_param_oids[i], &type, /*error=*/nullptr) !=
              NANOARROW_OK) {
        continue;
      }
    } else if (PostgresType::FromSchema(type_resolver, bind_schema.children[i], &type,
                                        /*error=*/nullptr) != NANOARROW_OK) {
      continue;
    }
    (*types)[i] = CastTypeName(type, bind_schema.children[i]);
  }
  return Status::Ok();
}

namespace {

// Prepare query with its parameters cast to types, reporting any error
// against query rather than the cast query, which the user never wrote.
Status PrepareCast(PGconn* conn, const std::string& query,
                   const std::vector<std::string>& types,
                   const std::vector<Oid>& param_oids) {
  const std::string cast_query = CastParameters(query, types, BackslashEscapes(conn));
  PGresult* result = PQprepare(conn, /*stmtName=*/"", cast_query.c_str(),
                               static_cast<int>(param_oids.size()), param_oids.data());
  Status status;
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    status = MakeStatus(result,
                        "Failed to prepare query with its parameters cast to their "
                        "bound types: {}\nQuery was: {}",
                        PQerrorMessage(conn), query.c_str());
  }
  PQclear(result);
  return status;
}

bool IsTextCast(const std::string& type) {
  return type == "text" || type.rfind("text[", 0) == 0;
}

// Prepare query with each parameter cast to its bound type, and set
// *cast_types to the casts it was prepared with.
Status PrepareWithParamCasts(PGconn* conn, const std::string& query,
                             const std::vector<Oid>& param_oids,
                             const struct ArrowSchema& bind_schema,
                             const PostgresTypeResolver& type_resolver,
                             const std::vector<Oid>& null_param_oids,
                             std::vector<std::string>* cast_types) {
  std::vector<std::string> types;
  UNWRAP_STATUS(ParameterCastTypes(type_resolver, bind_schema, null_param_oids, &types));

  // Leave strings uncast if that's enough: DuckDB won't compare a VARCHAR
  // expression with a DATE or UUID column, say, which an uncast string can be
  // inferred as.
  std::vector<std::string> non_text = types;
  bool any_text = false;
  bool any_non_text = false;
  for (std::string& type : non_text) {
    if (IsTextCast(type)) {
      type.clear();
      any_text = true;
    } else if (!type.empty()) {
      any_non_text = true;
    }
  }
  if (any_text && any_non_text) {
    Status status = PrepareCast(conn, query, non_text, param_oids);
    if (!IsAmbiguousParameterError(status)) {
      *cast_types = std::move(non_text);
      return status;
    }
  }
  *cast_types = types;
  return PrepareCast(conn, query, types, param_oids);
}

bool HasNullParams(const struct ArrowSchema& bind_schema) {
  for (int64_t i = 0; i < bind_schema.n_children; i++) {
    if (std::string_view(bind_schema.children[i]->format) == "n") return true;
  }
  return false;
}

}  // namespace

Status MotherduckResultReader::PrepareWithParamTypes() {
  // Skip the uncast prepare if resolving all-null parameters showed it fails.
  param_casts_.clear();
  if (!needs_param_casts_) {
    Status status = PqResultArrayReader::PrepareWithParamTypes();
    if (status.ok() || !IsAmbiguousParameterError(status)) return status;
  }
  return PrepareWithParamCasts(conn_, query_, bind_stream_->param_types,
                               bind_stream_->bind_schema.value, *type_resolver_,
                               /*null_param_oids=*/bind_stream_->param_types,
                               &param_casts_);
}

std::string MotherduckResultReader::ResultColumnName(std::string name) const {
  return param_casts_.empty() ? name : UncastParameters(name, param_casts_);
}

Status MotherduckResultReader::ResolveNullParamTypes() {
  Status status = PqResultArrayReader::ResolveNullParamTypes();
  if (status.ok() || !IsAmbiguousParameterError(status)) return status;
  needs_param_casts_ = true;
  // Cast the parameters that have types, so the server can infer the rest.
  std::vector<std::string> cast_types;
  status = PrepareWithParamCasts(conn_, query_, /*param_oids=*/{},
                                 bind_stream_->bind_schema.value, *type_resolver_,
                                 /*null_param_oids=*/{}, &cast_types);
  // If it still can't, the all-null parameters are bound with their Arrow types.
  if (IsAmbiguousParameterError(status)) return Status::Ok();
  UNWRAP_STATUS(status);
  return ReconcileWithPreparedParamTypes();
}

Status MotherduckResultReader::ExecuteAll(int64_t* affected_rows) {
  // For the case where we don't need a result, we either need to exhaust the bind
  // stream (if there is one) or execute the query without binding.
  if (bind_stream_) {
    UNWRAP_STATUS(bind_stream_->Begin([] { return Status::Ok(); }));

    UNWRAP_STATUS(ResolveNullParamTypes());

    // SetParamTypes() may begin a transaction, so from here on errors abort.
    UNWRAP_STATUS(AbortBindOnError(
        bind_stream_->SetParamTypes(conn_, *type_resolver_, autocommit_)));
    UNWRAP_STATUS(AbortBindOnError(PrepareWithParamTypes()));

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
    UNWRAP_STATUS(AbortBindOnError(bind_stream_->Cleanup(conn_)));
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
  param_casts_.clear();
  Status status = PostgresStatement::PrepareQuery(conn, query, param_oids, bind_schema);
  if (status.ok() || !IsAmbiguousParameterError(status)) return status;
  if (bind_schema == nullptr) {
    return Status::NotImplemented(
        "[libpq] MotherDuck can't infer the types of this query's parameters; bind "
        "parameters first, or cast them in the query (e.g. $1::INTEGER): ",
        status.message());
  }
  status = PrepareWithParamCasts(conn, query, param_oids, *bind_schema, type_resolver(),
                                 /*null_param_oids=*/{}, &param_casts_);
  if (!IsAmbiguousParameterError(status) || !HasNullParams(*bind_schema)) {
    return status;
  }
  // As when executing, cast all-null parameters the endpoint can't infer to text.
  const std::vector<Oid> text_oids(bind_schema->n_children,
                                   type_resolver().GetOID(PostgresTypeId::kText));
  return PrepareWithParamCasts(conn, query, param_oids, *bind_schema, type_resolver(),
                               text_oids, &param_casts_);
}

std::string MotherduckStatement::ResultColumnName(std::string name) const {
  return param_casts_.empty() ? name : UncastParameters(name, param_casts_);
}

// MotherDuck answers DDL with an empty result set instead of a command tag.
bool MotherduckStatement::IsCommandOk(PGresult* result) { return IsResultOk(result); }

}  // namespace adbcpq
