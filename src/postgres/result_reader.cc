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

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "result_reader.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "copy/reader.h"
#include "driver/common/utils.h"
#include "driver/framework/status.h"
#include "motherduck_pipeline.h"

namespace adbcpq {

PqResultArrayReader::~PqResultArrayReader() {
  // ReleaseConnection already finished the libpq calls while the connection
  // was still open. After PQfinish only the result memory may be freed.
  if (connection_ != nullptr && connection_->disconnected()) {
    helper_.DiscardLocal();
    ResetErrors();
    return;
  }
  if (!closed_) {
    const bool failed = helper_.Finish();
    const bool cancel = cancel_requested_.load(std::memory_order_acquire);
    // A clean drain of a finished script commits. A cancel or a failure does not.
    EndOwnedTransaction(!(failed || cancel));
  }
  ResetErrors();
}

int PqResultArrayReader::GetSchema(struct ArrowSchema* out) {
  ResetErrors();

  if (schema_->release == nullptr) {
    Status status = Initialize(nullptr);
    if (!status.ok()) {
      status.ToAdbc(&error_);
      return EINVAL;
    }
  }

  return ArrowSchemaDeepCopy(schema_.get(), out);
}

int PqResultArrayReader::GetNext(struct ArrowArray* out) {
  if (cancel_requested_.load(std::memory_order_acquire) && !closed_) {
    closed_ = true;
    helper_.Finish();
    EndOwnedTransaction(false);
    Status status(ADBC_STATUS_CANCELLED,
                  "[libpq] canceling statement due to user request");
    status.SetSqlState("57014");
    return Fail(status);
  }

  if (closed_) {
    if (error_.message == nullptr) {
      status_ = ADBC_STATUS_INVALID_STATE;
      InternalAdbcSetError(&error_, "%s",
                           "[libpq] Reader invalidated (statement or reader was closed)");
    }
    return EINVAL;
  }

  ResetErrors();
  status_ = ADBC_STATUS_OK;

  Status status;
  if (schema_->release == nullptr) {
    status = Initialize(nullptr);
    if (!status.ok()) {
      status.ToAdbc(&error_);
      return EINVAL;
    }
  }

  if (streaming_rows_) return GetNextStream(out);

  // If don't already have a result, populate it by binding the next row
  // in the bind stream. If this is the first call to GetNext(), we have
  // already populated the result.
  if (!helper_.HasResult()) {
    // If there was no bind stream provided or the existing bind stream has been
    // exhausted, we are done.
    if (!bind_stream_) {
      out->release = nullptr;
      return NANOARROW_OK;
    }

    // Keep binding and executing until we have a result to return
    status = BindNextAndExecute(nullptr);
    if (!status.ok()) {
      status.ToAdbc(&error_);
      return EIO;
    }

    // It's possible that there is still nothing to do here
    if (!helper_.HasResult()) {
      out->release = nullptr;
      return NANOARROW_OK;
    }
  }

  nanoarrow::UniqueArray tmp;
  NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromSchema(tmp.get(), schema_.get(), &na_error_));
  NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(tmp.get()));
  for (int i = 0; i < helper_.NumColumns(); i++) {
    NANOARROW_RETURN_NOT_OK(field_readers_[i]->InitArray(tmp->children[i]));
  }

  // TODO: If we get an EOVERFLOW here (e.g., big string data), we
  // would need to keep track of what row number we're on and start
  // from there instead of begin() on the next call. We could also
  // respect the size hint here to chunk the batches.
  struct ArrowBufferView item;
  for (auto it = helper_.begin(); it != helper_.end(); it++) {
    auto row = *it;
    for (int i = 0; i < helper_.NumColumns(); i++) {
      auto pg_item = row[i];
      item.data.data = pg_item.data;

      if (pg_item.is_null) {
        item.size_bytes = -1;
      } else {
        item.size_bytes = pg_item.len;
      }

      NANOARROW_RETURN_NOT_OK(field_readers_[i]->Read(
          &item, static_cast<int32_t>(item.size_bytes), tmp->children[i], &na_error_));
    }
  }

  for (int i = 0; i < helper_.NumColumns(); i++) {
    NANOARROW_RETURN_NOT_OK(field_readers_[i]->FinishArray(tmp->children[i], &na_error_));
  }

  tmp->length = helper_.NumRows();
  tmp->null_count = 0;
  NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(tmp.get(), &na_error_));

  // Signal that the next call to GetNext() will have to populate the result again
  helper_.ClearResult();

  // Canonically return zero-size results as an empty stream
  if (tmp->length == 0) {
    out->release = nullptr;
    return NANOARROW_OK;
  }

  ArrowArrayMove(tmp.get(), out);
  return NANOARROW_OK;
}

const char* PqResultArrayReader::GetLastError() {
  if (error_.message != nullptr) {
    return error_.message;
  } else {
    return na_error_.message;
  }
}

Status PqResultArrayReader::Initialize(int64_t* rows_affected) {
  helper_.set_output_format(PqResultHelper::Format::kBinary);
  helper_.set_param_format(PqResultHelper::Format::kBinary);

  // If we have to do binding, set up the bind stream an execute until
  // there is a result with more than zero rows to populate.
  if (bind_stream_) {
    UNWRAP_STATUS(bind_stream_->Begin([] { return Status::Ok(); }));

    UNWRAP_STATUS(ResolveNullParamTypes());

    // Now set parameter types (will use reconciled types for NA fields)
    UNWRAP_STATUS(bind_stream_->SetParamTypes(conn_, *type_resolver_, autocommit_));

    // Re-prepare with the actual parameter types
    UNWRAP_STATUS(PrepareWithParamTypes());

    UNWRAP_STATUS(BindNextAndExecute(nullptr));

    // If there were no arrays in the bind stream, we still need a result
    // to populate the schema. If there were any arrays in the bind stream,
    // the last one will still be in helper_ even if it had zero rows.
    if (!helper_.HasResult()) {
      UNWRAP_STATUS(helper_.DescribePrepared());
    }

    // We can't provide affected row counts if there is a bind stream and
    // an output because we don't know how many future bind arrays/rows there
    // might be.
    if (rows_affected != nullptr) {
      *rows_affected = -1;
    }
  } else {
    UNWRAP_STATUS(OpenRowStream());
    if (helper_.streaming()) {
      Status status = helper_.NextPacket();
      if (!status.ok()) {
        // MotherDuck keeps the transaction open after a catalog error, so
        // this has to roll back. The destructor would otherwise commit it.
        EndOwnedTransaction(false);
        return status;
      }
      if (!helper_.HasResult()) {
        EndOwnedTransaction(false);
        return Status::IO("[libpq] Query returned no result");
      }
    }
    if (!helper_.streaming()) {
      // The server is done, including a command whose only leftover was a
      // buffered trailer. Commit before returning so the change is visible.
      UNWRAP_STATUS(EndOwnedTransaction(true));
    }
    if (helper_.streaming() || helper_.NumRows() > 0) {
      // More packets are coming, or this packet still has rows to split on
      // the byte hint. The count stays unknown: it is not written later.
      streaming_rows_ = true;
      if (rows_affected != nullptr) *rows_affected = -1;
    } else if (rows_affected != nullptr) {
      *rows_affected = helper_.AffectedRows();
    }
  }

  // Build the schema for which we are about to build results
  ArrowSchemaInit(schema_.get());
  UNWRAP_NANOARROW(na_error_, Internal,
                   ArrowSchemaSetTypeStruct(schema_.get(), helper_.NumColumns()));

  for (int i = 0; i < helper_.NumColumns(); i++) {
    PostgresType child_type;
    UNWRAP_ERRNO(Internal,
                 type_resolver_->FindWithDefault(helper_.FieldType(i), &child_type));

    child_type = child_type.WithFieldName(ResultColumnName(helper_.FieldName(i)))
                     .WithTypeModifier(helper_.FieldTypeModifier(i));
    UNWRAP_ERRNO(Internal, child_type.SetSchema(schema_->children[i], vendor_name_));

    std::unique_ptr<PostgresCopyFieldReader> child_reader;
    UNWRAP_NANOARROW(
        na_error_, Internal,
        MakeCopyFieldReader(child_type, schema_->children[i], &child_reader, &na_error_));

    child_reader->Init(child_type);
    UNWRAP_NANOARROW(na_error_, Internal, child_reader->InitSchema(schema_->children[i]));

    field_readers_.push_back(std::move(child_reader));
  }

  return Status::Ok();
}

namespace {

struct ResultStream {
  std::shared_ptr<PqResultArrayReader> reader;

  explicit ResultStream(std::shared_ptr<PqResultArrayReader> reader)
      : reader(std::move(reader)) {}

  int GetSchema(struct ArrowSchema* out) { return reader->GetSchema(out); }
  int GetNext(struct ArrowArray* out) { return reader->GetNext(out); }
  const char* GetLastError() { return reader->GetLastError(); }
};

void ReleaseResultStream(struct ArrowArrayStream* stream) {
  delete static_cast<ResultStream*>(stream->private_data);
  stream->private_data = nullptr;
  stream->release = nullptr;
}

int64_t RowBytes(PqResultRow row, int columns) {
  int64_t bytes = 1;
  for (int column = 0; column < columns; column++) {
    PqRecord value = row[column];
    if (!value.is_null) bytes += value.len;
  }
  return bytes;
}

}  // namespace

Status PqResultArrayReader::PrepareWithParamTypes() {
  return helper_.Prepare(bind_stream_->param_types);
}

Status PqResultArrayReader::ResolveNullParamTypes() {
  const bool has_na_params = std::any_of(
      bind_stream_->bind_schema_fields.begin(), bind_stream_->bind_schema_fields.end(),
      [](const ArrowSchemaView& view) { return view.type == NANOARROW_TYPE_NA; });
  if (!has_na_params) return Status::Ok();

  // Prepare WITHOUT parameter types to let PostgreSQL infer them. This is required
  // to resolve Arrow NA (all-null) parameters to the expected PostgreSQL types.
  UNWRAP_STATUS(helper_.Prepare());
  return ReconcileWithPreparedParamTypes();
}

Status PqResultArrayReader::ReconcileWithPreparedParamTypes() {
  // Get PostgreSQL's expected parameter types
  UNWRAP_STATUS(helper_.DescribePrepared());
  PostgresType expected_types;
  UNWRAP_STATUS(helper_.ResolveParamTypes(*type_resolver_, &expected_types));

  // Reconcile Arrow schema with expected types
  return bind_stream_->ReconcileWithExpectedTypes(expected_types);
}

Status PqResultArrayReader::ToArrayStream(int64_t* affected_rows,
                                          struct ArrowArrayStream* out) {
  if (out == nullptr) {
    // If there is no output requested, we still need to execute and
    // set affected_rows if needed. We don't need an output schema or to set up a copy
    // reader, so we can skip those steps by going straight to Execute(). This also
    // enables us to support queries with multiple statements because we can call PQexec()
    // instead of PQexecParams().
    UNWRAP_STATUS(ExecuteAll(affected_rows));
    return Status::Ok();
  }

  // Otherwise, execute until we have a result to return. We need this to provide row
  // counts for DELETE and CREATE TABLE queries as well as to provide more informative
  // errors until this reader class is wired up to provide extended AdbcError information.
  UNWRAP_STATUS(Initialize(affected_rows));

  nanoarrow::ArrayStreamFactory<ResultStream>::InitArrayStream(
      new ResultStream(shared_from_this()), out);
  // Replace the factory release so this stream can be recognized without
  // reading another stream's private data.
  out->release = &ReleaseResultStream;

  return Status::Ok();
}

Status PqResultArrayReader::EndOwnedTransaction(bool commit) {
  if (!own_txn_) return Status::Ok();
  own_txn_ = false;
  // A failed statement aborts the transaction. COMMIT would be rejected.
  if (commit && PQtransactionStatus(conn_) == PQTRANS_INERROR) commit = false;
  const char* sql = commit ? "COMMIT" : "ROLLBACK";
  PGresult* result = PQexec(conn_, sql);
  if (result == nullptr || !IsResultOk(result)) {
    Status status = MakeStatus(result, "[libpq] Failed to execute query '{}': {}", sql,
                               PQerrorMessage(conn_));
    PQclear(result);
    return status;
  }
  PQclear(result);
  return Status::Ok();
}

Status PqResultArrayReader::OpenRowStream() {
  std::vector<std::string> statements =
      SplitStatements(helper_.query(), BackslashEscapes(conn_));
  if (statements.size() <= 1) return helper_.BeginRowStream();

  // The result readers decode binary values, and only the extended protocol
  // can ask for those. It also accepts one statement. An explicit transaction
  // keeps the earlier statements from committing when a later one fails.
  // The connection's flag wins. The member defaults to on so a caller that
  // never sets it still gets one transaction.
  const bool autocommit =
      connection_ != nullptr ? connection_->autocommit() : autocommit_;
  if (autocommit) {
    Status begin = helper_.ExecuteCancellable("BEGIN");
    if (!begin.ok()) return begin;
    own_txn_ = true;
  }

  for (size_t i = 0; i + 1 < statements.size(); i++) {
    Status status = helper_.ExecuteCancellable(statements[i]);
    if (!status.ok()) {
      EndOwnedTransaction(false);
      return status;
    }
  }

  helper_.set_query(std::move(statements.back()));
  Status status = helper_.BeginRowStream();
  if (!status.ok()) EndOwnedTransaction(false);
  return status;
}

void PqResultArrayReader::ReleaseConnection() {
  if (closed_) return;
  if (!helper_.streaming()) {
    // Rows are already local. Leave them readable and commit any script that
    // finished on the server.
    EndOwnedTransaction(true);
    return;
  }
  closed_ = true;
  const bool failed = helper_.Finish();
  const bool cancel = cancel_requested_.load(std::memory_order_acquire);
  EndOwnedTransaction(!(failed || cancel));
  status_ = ADBC_STATUS_INVALID_STATE;
  InternalAdbcSetError(&error_, "%s",
                       "[libpq] Reader invalidated (statement or reader was closed)");
}

AdbcStatusCode PqResultArrayReader::RequestCancel(struct AdbcError* error) {
  (void)error;
  cancel_requested_.store(true, std::memory_order_release);
  return ADBC_STATUS_OK;
}

Status PqResultArrayReader::BindNextAndExecute(int64_t* affected_rows) {
  // Keep pulling from the bind stream and executing as long as
  // we receive results with zero rows.
  do {
    UNWRAP_STATUS(bind_stream_->EnsureNextRow());

    if (!bind_stream_->current->release) {
      UNWRAP_STATUS(bind_stream_->Cleanup(conn_));
      bind_stream_.reset();
      return Status::Ok();
    }

    PGresult* result;
    Status status = bind_stream_->BindAndExecuteCurrentRow(
        conn_, &result, /*result_format*/ kPgBinaryFormat);
    if (!status.ok()) {
      bind_stream_->Abort(conn_);
      bind_stream_.reset();
      return status;
    }
    helper_.SetResult(result);
    if (affected_rows) {
      (*affected_rows) += helper_.AffectedRows();
    }
  } while (helper_.NumRows() == 0);

  return Status::Ok();
}

Status PqResultArrayReader::ExecuteAll(int64_t* affected_rows) {
  // For the case where we don't need a result, we either need to exhaust the bind
  // stream (if there is one) or execute the query without binding.
  if (bind_stream_) {
    UNWRAP_STATUS(bind_stream_->Begin([] { return Status::Ok(); }));

    UNWRAP_STATUS(ResolveNullParamTypes());

    UNWRAP_STATUS(bind_stream_->SetParamTypes(conn_, *type_resolver_, autocommit_));
    UNWRAP_STATUS(PrepareWithParamTypes());

    // Reset affected rows to zero before binding and executing any
    if (affected_rows) {
      (*affected_rows) = 0;
    }

    do {
      UNWRAP_STATUS(BindNextAndExecute(affected_rows));
    } while (bind_stream_);
  } else {
    UNWRAP_STATUS(helper_.Execute());

    if (affected_rows != nullptr) {
      *affected_rows = helper_.AffectedRows();
    }
  }

  return Status::Ok();
}

int PqResultArrayReader::Fail(const Status& status) {
  status_ = status.ToAdbc(&error_);
  return InternalAdbcStatusCodeToErrno(status_);
}

int PqResultArrayReader::FailArrow(int code) {
  if (code == NANOARROW_OK) return NANOARROW_OK;
  status_ = ADBC_STATUS_INTERNAL;
  InternalAdbcSetError(&error_, "[libpq] %s",
                       na_error_.message[0] != '\0' ? na_error_.message
                                                    : "failed to build result batch");
  return code;
}

int PqResultArrayReader::GetNextStream(struct ArrowArray* out) {
  nanoarrow::UniqueArray tmp;
  bool started = false;
  int64_t bytes = 0;
  int64_t nrows = 0;
  // The current PGresult is cleared once its rows are consumed, so the schema
  // is what still knows how many columns the batch has.
  const int columns = static_cast<int>(schema_->n_children);

  while (true) {
    if (!helper_.HasResult() || next_row_ >= helper_.NumRows()) {
      Status status = helper_.NextPacket();
      next_row_ = 0;
      if (!status.ok()) {
        EndOwnedTransaction(false);
        return Fail(status);
      }
      if (!helper_.HasResult() || helper_.NumRows() == 0) break;
    }

    if (!started) {
      if (int code = FailArrow(
              ArrowArrayInitFromSchema(tmp.get(), schema_.get(), &na_error_))) {
        return code;
      }
      if (int code = FailArrow(ArrowArrayStartAppending(tmp.get()))) return code;
      for (int i = 0; i < columns; i++) {
        if (int code = FailArrow(field_readers_[i]->InitArray(tmp->children[i]))) {
          return code;
        }
      }
      started = true;
    }

    PqResultRow row = helper_.Row(next_row_);
    struct ArrowBufferView item;
    for (int i = 0; i < columns; i++) {
      PqRecord value = row[i];
      item.data.data = value.data;
      item.size_bytes = value.is_null ? -1 : value.len;
      if (int code = FailArrow(field_readers_[i]->Read(
              &item, static_cast<int32_t>(item.size_bytes), tmp->children[i],
              &na_error_))) {
        return code;
      }
    }
    bytes += RowBytes(row, columns);
    next_row_++;
    nrows++;
    if (bytes >= batch_size_hint_bytes_) break;
  }

  if (!helper_.streaming()) {
    if (Status end = EndOwnedTransaction(true); !end.ok()) return Fail(end);
  }

  if (nrows == 0) {
    out->release = nullptr;
    return NANOARROW_OK;
  }

  for (int i = 0; i < columns; i++) {
    if (int code =
            FailArrow(field_readers_[i]->FinishArray(tmp->children[i], &na_error_))) {
      return code;
    }
  }
  tmp->length = nrows;
  tmp->null_count = 0;
  if (int code = FailArrow(ArrowArrayFinishBuildingDefault(tmp.get(), &na_error_))) {
    return code;
  }
  ArrowArrayMove(tmp.get(), out);
  return NANOARROW_OK;
}

const struct AdbcError* PqResultArrayReader::ErrorFromArrayStream(
    struct ArrowArrayStream* stream, AdbcStatusCode* status) {
  if (stream == nullptr || stream->private_data == nullptr ||
      stream->release != &ReleaseResultStream) {
    return nullptr;
  }
  auto* holder = static_cast<ResultStream*>(stream->private_data);
  if (status != nullptr) *status = holder->reader->status_;
  return holder->reader->error_.message != nullptr ? &holder->reader->error_ : nullptr;
}

}  // namespace adbcpq
