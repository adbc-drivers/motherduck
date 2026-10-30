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

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>

#include "copy/writer.h"
#include "error.h"
#include "postgres_type.h"
#include "postgres_util.h"
#include "result_helper.h"

namespace adbcpq {

/// The flag indicating to PostgreSQL that we want binary-format values.
constexpr int kPgBinaryFormat = 1;

/// Helper to manage bind parameters with a prepared statement
struct BindStream {
  Handle<struct ArrowArrayStream> bind;
  Handle<struct ArrowArrayView> array_view;
  Handle<struct ArrowArray> current;
  Handle<struct ArrowSchema> bind_schema;
  int64_t current_row = -1;

  std::vector<struct ArrowSchemaView> bind_schema_fields;
  std::vector<std::unique_ptr<PostgresCopyFieldWriter>> bind_field_writers;

  // OIDs for parameter types
  std::vector<uint32_t> param_types;
  std::vector<char*> param_values;
  std::vector<int> param_formats;
  std::vector<int> param_lengths;
  Handle<struct ArrowBuffer> param_buffer;

  bool has_tz_field = false;
  bool autocommit = false;
  std::string tz_setting;

  // Expected types from PostgreSQL (after DESCRIBE); used to resolve NA params
  PostgresType expected_param_types;
  bool has_expected_types = false;

  struct ArrowError na_error;

  BindStream() {
    this->bind->release = nullptr;
    std::memset(&na_error, 0, sizeof(na_error));
  }

  void SetBind(struct ArrowArrayStream* stream) {
    this->bind.reset();
    ArrowArrayStreamMove(stream, &bind.value);
  }

  Status ReconcileWithExpectedTypes(const PostgresType& expected_types) {
    if (bind_schema->release == nullptr) {
      return Status::InvalidState("[libpq] Bind stream schema not initialized");
    }
    if (expected_types.n_children() != bind_schema->n_children) {
      return Status::InvalidState("[libpq] Expected ", expected_types.n_children(),
                                  " parameters but bind stream has ",
                                  bind_schema->n_children);
    }
    expected_param_types = expected_types;
    has_expected_types = true;
    return Status::Ok();
  }

  template <typename Callback>
  Status Begin(Callback&& callback) {
    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowArrayStreamGetSchema(&bind.value, &bind_schema.value, &na_error));

    struct ArrowSchemaView bind_schema_view;
    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowSchemaViewInit(&bind_schema_view, &bind_schema.value, &na_error));
    if (bind_schema_view.type != ArrowType::NANOARROW_TYPE_STRUCT) {
      return Status::InvalidState("[libpq] Bind parameters must have type STRUCT");
    }

    bind_schema_fields.resize(bind_schema->n_children);
    for (size_t i = 0; i < bind_schema_fields.size(); i++) {
      UNWRAP_ERRNO(Internal,
                   ArrowSchemaViewInit(&bind_schema_fields[i], bind_schema->children[i],
                                       /*error*/ nullptr));
    }

    UNWRAP_NANOARROW(
        na_error, Internal,
        ArrowArrayViewInitFromSchema(&array_view.value, &bind_schema.value, &na_error));

    ArrowBufferInit(&param_buffer.value);

    return std::move(callback)();
  }

  Status SetParamTypes(PGconn* pg_conn, const PostgresTypeResolver& type_resolver,
                       const bool autocommit) {
    param_types.resize(bind_schema->n_children);
    param_values.resize(bind_schema->n_children);
    param_lengths.resize(bind_schema->n_children);
    param_formats.resize(bind_schema->n_children, kPgBinaryFormat);
    bind_field_writers.resize(bind_schema->n_children);

    for (size_t i = 0; i < bind_field_writers.size(); i++) {
      PostgresType type;

      // Handle NA type by using expected parameter type from PostgreSQL
      if (has_expected_types && bind_schema_fields[i].type == NANOARROW_TYPE_NA &&
          i < static_cast<size_t>(expected_param_types.n_children())) {
        const auto& expected_type = expected_param_types.child(i);
        // If PostgreSQL couldn't infer a concrete type (e.g., SELECT $1), don't
        // force an "expected" type; fall back to Arrow-derived mapping.
        if (expected_type.oid() != 0 &&
            expected_type.type_id() != PostgresTypeId::kUnknown) {
          type = expected_type;
        } else {
          UNWRAP_NANOARROW(
              na_error, Internal,
              PostgresType::FromSchema(type_resolver, bind_schema->children[i], &type,
                                       &na_error));
        }
      } else {
        // Normal case: derive type from Arrow schema
        UNWRAP_NANOARROW(na_error, Internal,
                         PostgresType::FromSchema(type_resolver, bind_schema->children[i],
                                                  &type, &na_error));
      }

      // tz-aware timestamps require special handling to set the timezone to UTC
      // prior to sending over the binary protocol; must be reset after execute
      if (!has_tz_field && type.type_id() == PostgresTypeId::kTimestamptz) {
        UNWRAP_STATUS(SetDatabaseTimezoneUTC(pg_conn, autocommit));
        has_tz_field = true;
        this->autocommit = autocommit;
      }

      std::unique_ptr<PostgresCopyFieldWriter> writer;
      UNWRAP_NANOARROW(
          na_error, Internal,
          MakeCopyFieldWriter(bind_schema->children[i], array_view->children[i],
                              type_resolver, nullptr, &writer,
                              /*disable_decimal_fast_path=*/false, &na_error));

      param_types[i] = type.oid();
      param_formats[i] = kPgBinaryFormat;
      bind_field_writers[i] = std::move(writer);
    }

    return Status::Ok();
  }

  Status SetDatabaseTimezoneUTC(PGconn* pg_conn, const bool autocommit) {
    if (autocommit) {
      PqResultHelper helper(pg_conn, "BEGIN");
      UNWRAP_STATUS(helper.Execute());
    }

    PqResultHelper get_tz(pg_conn, "SELECT current_setting('TIMEZONE')");
    UNWRAP_STATUS(get_tz.Execute());
    for (auto row : get_tz) {
      tz_setting = row[0].value();
    }

    PqResultHelper set_utc(pg_conn, "SET TIME ZONE 'UTC'");
    UNWRAP_STATUS(set_utc.Execute());

    return Status::Ok();
  }

  Status Prepare(PGconn* pg_conn, const std::string& query) {
    PqResultHelper helper(pg_conn, query);
    UNWRAP_STATUS(helper.Prepare(param_types));
    return Status::Ok();
  }

  Status PullNextArray() {
    if (current->release != nullptr) ArrowArrayRelease(&current.value);

    UNWRAP_NANOARROW(na_error, IO,
                     ArrowArrayStreamGetNext(&bind.value, &current.value, &na_error));

    if (current->release != nullptr) {
      UNWRAP_NANOARROW(
          na_error, Internal,
          ArrowArrayViewSetArray(&array_view.value, &current.value, &na_error));
    }

    return Status::Ok();
  }

  Status EnsureNextRow() {
    if (current->release != nullptr) {
      current_row++;
      if (current_row < current->length) {
        return Status::Ok();
      }
    }

    // Pull until we have an array with at least one row or the stream is finished
    do {
      UNWRAP_STATUS(PullNextArray());
      if (current->release == nullptr) {
        current_row = -1;
        return Status::Ok();
      }
    } while (current->length == 0);

    current_row = 0;
    return Status::Ok();
  }

  // Serialize the current row into param_values and param_lengths.
  Status SerializeCurrentRow() {
    param_buffer->size_bytes = 0;
    int64_t last_offset = 0;
    std::vector<bool> is_null_param(array_view->n_children);

    for (int64_t col = 0; col < array_view->n_children; col++) {
      is_null_param[col] = ArrowArrayViewIsNull(array_view->children[col], current_row);

      // Safety check: NA type arrays should only contain nulls
      if (bind_schema_fields[col].type == NANOARROW_TYPE_NA && !is_null_param[col]) {
        return Status::InvalidArgument(
            "Parameter $", col + 1,
            " has null type but contains a non-null value at row ", current_row);
      }

      if (!is_null_param[col]) {
        // Note that this Write() call currently writes the (int32_t) byte size of the
        // field in addition to the serialized value.
        UNWRAP_NANOARROW(
            na_error, Internal,
            bind_field_writers[col]->Write(&param_buffer.value, current_row, &na_error));
      } else {
        UNWRAP_ERRNO(Internal, ArrowBufferAppendInt32(&param_buffer.value, 0));
      }

      int64_t param_length = param_buffer->size_bytes - last_offset - sizeof(int32_t);
      if (param_length > (std::numeric_limits<int>::max)()) {
        return Status::Internal("Parameter ", col, "serialized to >2GB of binary");
      }

      param_lengths[col] = static_cast<int>(param_length);
      last_offset = param_buffer->size_bytes;
    }

    last_offset = 0;
    for (int64_t col = 0; col < array_view->n_children; col++) {
      last_offset += sizeof(int32_t);
      if (is_null_param[col]) {
        param_values[col] = nullptr;
      } else {
        param_values[col] = reinterpret_cast<char*>(param_buffer->data) + last_offset;
      }
      last_offset += param_lengths[col];
    }

    return Status::Ok();
  }

  Status BindAndExecuteCurrentRow(PGconn* pg_conn, PGresult** result_out,
                                  int result_format) {
    UNWRAP_STATUS(SerializeCurrentRow());

    PGresult* result =
        PQexecPrepared(pg_conn, /*stmtName=*/"",
                       /*nParams=*/bind_schema->n_children, param_values.data(),
                       param_lengths.data(), param_formats.data(), result_format);

    ExecStatusType pg_status = PQresultStatus(result);
    if (pg_status != PGRES_COMMAND_OK && pg_status != PGRES_TUPLES_OK) {
      Status status =
          MakeStatus(result, "[libpq] Failed to execute prepared statement: {} {}",
                     PQresStatus(pg_status), PQerrorMessage(pg_conn));
      PQclear(result);
      return status;
    }

    *result_out = result;
    return Status::Ok();
  }

  // Queue the current row for the prepared statement in a pipeline. libpq
  // copies the parameters, so the next row can reuse the buffer.
  Status SendCurrentRow(PGconn* pg_conn, int result_format) {
    UNWRAP_STATUS(SerializeCurrentRow());

    if (PQsendQueryPrepared(pg_conn, /*stmtName=*/"",
                            /*nParams=*/bind_schema->n_children, param_values.data(),
                            param_lengths.data(), param_formats.data(),
                            result_format) != 1) {
      return MakeStatus(nullptr, "[libpq] Failed to send prepared statement: {}",
                        PQerrorMessage(pg_conn));
    }
    return Status::Ok();
  }

  // Undo SetDatabaseTimezoneUTC after a failure. Under autocommit, rolling
  // back the transaction it began also restores the time zone. Otherwise the
  // user's transaction has failed, and rolling it back restores it.
  void Abort(PGconn* pg_conn) {
    if (has_tz_field && autocommit) {
      PqResultHelper rollback(pg_conn, "ROLLBACK");
      // The failure that got us here is the error worth reporting.
      (void)rollback.Execute();
    }
  }

  Status Cleanup(PGconn* pg_conn) {
    if (has_tz_field) {
      PqResultHelper reset(pg_conn, "SET TIME ZONE '" + tz_setting + "'");
      UNWRAP_STATUS(reset.Execute());

      if (autocommit) {
        // SetDatabaseTimezoneUTC will start a transaction if autocommit is
        // enabled (so the timezone setting will roll back if we error), so we
        // need to commit here.  Otherwise we should not commit and let the
        // user commit.
        PqResultHelper commit(pg_conn, "COMMIT");
        UNWRAP_STATUS(commit.Execute());
      }
    }

    return Status::Ok();
  }

  // Whether to declare a parameter as its target column's type rather than
  // its Arrow type's: where MakeCopyFieldWriter encodes as the column's type
  // (numeric, jsonb and arrays), and for timestamps, whose encodings match,
  // so the value lands as COPY would have stored it rather than being cast
  // through the session time zone.
  static bool DeclareAsTarget(const PostgresType& type, const PostgresType& target) {
    switch (target.type_id()) {
      case PostgresTypeId::kNumeric:
        return true;
      case PostgresTypeId::kArray:
        return type.type_id() == PostgresTypeId::kArray;
      case PostgresTypeId::kJsonb:
        return type.type_id() == PostgresTypeId::kJson;
      case PostgresTypeId::kTimestamp:
      case PostgresTypeId::kTimestamptz:
        return type.type_id() == PostgresTypeId::kTimestamp ||
               type.type_id() == PostgresTypeId::kTimestamptz;
      default:
        return false;
    }
  }

  // Serialize the current row's values into param_buffer for the parameters
  // starting at first_param, recording each one's offset (-1 for null) and
  // length.
  Status AppendCurrentRow(int64_t first_param, std::vector<int64_t>* offsets,
                          std::vector<int>* lengths) {
    for (int64_t col = 0; col < array_view->n_children; col++) {
      const int64_t param = first_param + col;
      if (ArrowArrayViewIsNull(array_view->children[col], current_row)) {
        (*offsets)[param] = -1;
        (*lengths)[param] = 0;
        continue;
      }
      // Write() prefixes the value with its int32 length, as in COPY.
      const int64_t start = param_buffer->size_bytes + sizeof(int32_t);
      UNWRAP_NANOARROW(
          na_error, Internal,
          bind_field_writers[col]->Write(&param_buffer.value, current_row, &na_error));
      const int64_t length = param_buffer->size_bytes - start;
      if (length > (std::numeric_limits<int>::max)()) {
        return Status::Internal("Column ", col, " serialized to >2GB of binary");
      }
      (*offsets)[param] = start;
      (*lengths)[param] = static_cast<int>(length);
    }
    return Status::Ok();
  }

  // Bulk ingest without COPY FROM STDIN, which MotherDuck's endpoint doesn't
  // support (#31): send the rows as multi-row INSERTs with binary parameters,
  // all through one pipeline so they run in one implicit transaction.
  Status ExecuteInsert(PGconn* pg_conn, const PostgresTypeResolver& type_resolver,
                       const std::string& escaped_table,
                       const std::string& escaped_field_list,
                       const PostgresType* target_types, bool disable_decimal_fast_path,
                       int64_t* rows_affected) {
    if (rows_affected) *rows_affected = 0;

    const int64_t n_cols = bind_schema->n_children;
    if (n_cols == 0) {
      return Status::NotImplemented("[libpq] Bulk ingest requires at least one column");
    }

    // The protocol counts parameters in an int16.
    constexpr int64_t kMaxParams = 65535;
    if (n_cols > kMaxParams) {
      return Status::NotImplemented("[libpq] Bulk ingest supports at most ", kMaxParams,
                                    " columns, not ", n_cols);
    }

    // Encode each column as COPY would, and declare the type it is encoded as.
    // The server casts that to the column's type where they differ.
    std::vector<Oid> row_types(n_cols);
    bind_field_writers.resize(n_cols);
    for (int64_t i = 0; i < n_cols; i++) {
      const PostgresType* target_type = nullptr;
      if (target_types != nullptr && i < target_types->n_children()) {
        target_type = &target_types->child(i);
      }
      UNWRAP_NANOARROW(
          na_error, Internal,
          MakeCopyFieldWriter(bind_schema->children[i], array_view->children[i],
                              type_resolver, target_type, &bind_field_writers[i],
                              disable_decimal_fast_path, &na_error));

      PostgresType type;
      UNWRAP_NANOARROW(na_error, Internal,
                       PostgresType::FromSchema(type_resolver, bind_schema->children[i],
                                                &type, &na_error));
      if (target_type != nullptr && DeclareAsTarget(type, *target_type)) {
        type = *target_type;
      }
      row_types[i] = type.oid();
    }

    // MotherDuck rejects protocol messages over 16 MiB, so cap each batch's
    // values below that, leaving room for the rest of the Bind message.
    constexpr int64_t kMaxInsertBytes = 15 * 1024 * 1024;
    constexpr int64_t kMaxRowsPerInsert = 1000;
    const int64_t rows_per_insert = (std::min)(kMaxRowsPerInsert, kMaxParams / n_cols);

    const std::string insert_prefix =
        "INSERT INTO " + escaped_table + " (" + escaped_field_list + ") VALUES ";
    auto make_insert = [&](int64_t n_rows) {
      std::string query = insert_prefix;
      int64_t param = 1;
      for (int64_t row = 0; row < n_rows; row++) {
        query += row == 0 ? "(" : ", (";
        for (int64_t col = 0; col < n_cols; col++) {
          if (col > 0) query += ", ";
          query += "$" + std::to_string(param++);
        }
        query += ")";
      }
      return query;
    };

    std::vector<Oid> types;
    types.reserve(rows_per_insert * n_cols);
    for (int64_t row = 0; row < rows_per_insert; row++) {
      types.insert(types.end(), row_types.begin(), row_types.end());
    }
    std::vector<int> formats(types.size(), kPgBinaryFormat);
    std::vector<int64_t> offsets(types.size());  // -1 for null
    std::vector<int> lengths(types.size());
    std::vector<const char*> values(types.size());

    int64_t rows_sent = 0;
    bool finished = false;
    bool row_pending = false;  // the current row didn't fit in the last batch
    // Full batches all share one statement, so parse it once: the first full
    // batch is held back while the statement is prepared, then sent with it.
    bool prepared = false;
    int64_t held_rows = 0;
    Status status;
    Status pipeline_status = RunPipeline(
        pg_conn,
        [&](bool* sent) -> Status {
          *sent = true;
          int64_t n_rows = held_rows;
          held_rows = 0;
          if (n_rows == 0) {
            // Serialize up to rows_per_insert rows, which may span arrays.
            param_buffer->size_bytes = 0;
            while (!finished && n_rows < rows_per_insert) {
              if (!row_pending) {
                UNWRAP_STATUS(EnsureNextRow());
                if (current->release == nullptr) {
                  finished = true;
                  break;
                }
              }
              row_pending = false;
              const int64_t row_start = param_buffer->size_bytes;
              UNWRAP_STATUS(AppendCurrentRow(n_rows * n_cols, &offsets, &lengths));
              if (param_buffer->size_bytes > kMaxInsertBytes && n_rows > 0) {
                // Send this row with the next batch instead.
                param_buffer->size_bytes = row_start;
                row_pending = true;
                break;
              }
              n_rows++;
            }
            if (n_rows == 0) {
              *sent = false;
              return Status::Ok();
            }

            if (n_rows == rows_per_insert && !prepared) {
              if (PQsendPrepare(pg_conn, /*stmtName=*/"",
                                make_insert(rows_per_insert).c_str(),
                                static_cast<int>(types.size()), types.data()) != 1) {
                return MakeStatus(nullptr, "[libpq] Failed to prepare INSERT: {}",
                                  PQerrorMessage(pg_conn));
              }
              prepared = true;
              held_rows = n_rows;
              return Status::Ok();
            }
          }

          // The buffer may have moved while growing, so point into it only now.
          const int n_params = static_cast<int>(n_rows * n_cols);
          for (int i = 0; i < n_params; i++) {
            values[i] =
                offsets[i] < 0
                    ? nullptr
                    : reinterpret_cast<const char*>(param_buffer->data) + offsets[i];
          }
          // libpq copies the parameters, so the buffer can be reused. A
          // partial batch replaces the unnamed prepared statement, so the next
          // full batch prepares it again.
          int sent_ok;
          if (n_rows == rows_per_insert) {
            sent_ok =
                PQsendQueryPrepared(pg_conn, /*stmtName=*/"", n_params, values.data(),
                                    lengths.data(), formats.data(), kPgBinaryFormat);
          } else {
            prepared = false;
            sent_ok = PQsendQueryParams(pg_conn, make_insert(n_rows).c_str(), n_params,
                                        types.data(), values.data(), lengths.data(),
                                        formats.data(), kPgBinaryFormat);
          }
          if (sent_ok != 1) {
            return MakeStatus(nullptr, "[libpq] Failed to send INSERT: {}",
                              PQerrorMessage(pg_conn));
          }
          rows_sent += n_rows;
          return Status::Ok();
        },
        [&](size_t, PGresult* result) {
          // A failure aborts the statements after it; report only the first.
          const ExecStatusType result_status = PQresultStatus(result);
          if (!IsResultOk(result) && result_status != PGRES_PIPELINE_ABORTED &&
              status.ok()) {
            status = MakeStatus(result, "[libpq] Failed to execute INSERT: {} {}",
                                PQresStatus(result_status), PQresultErrorMessage(result));
          }
          PQclear(result);
        });

    // A statement's own error explains more than the pipeline failing after it.
    UNWRAP_STATUS(status);
    UNWRAP_STATUS(pipeline_status);
    if (rows_affected) *rows_affected = rows_sent;
    return Status::Ok();
  }
};
}  // namespace adbcpq
