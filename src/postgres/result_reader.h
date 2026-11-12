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

#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "bind_stream.h"
#include "connection.h"
#include "copy/reader.h"
#include "result_helper.h"

namespace adbcpq {

class PqResultArrayReader : public std::enable_shared_from_this<PqResultArrayReader> {
 public:
  PqResultArrayReader(PGconn* conn, std::shared_ptr<PostgresTypeResolver> type_resolver,
                      std::string query)
      : conn_(conn),
        helper_(conn, std::move(query)),
        type_resolver_(type_resolver),
        autocommit_(true) {
    ArrowErrorInit(&na_error_);
    error_ = ADBC_ERROR_INIT;
    helper_.SetCancelFlag(&cancel_requested_);
  }

  virtual ~PqResultArrayReader();

  // Ensure the reader knows what the autocommit status was on creation. This is used
  // so that the temporary timezone setting required for parameter binding can be wrapped
  // in a transaction (or not) accordingly.
  void SetAutocommit(bool autocommit) { autocommit_ = autocommit; }

  void SetBind(struct ArrowArrayStream* stream) {
    bind_stream_ = std::make_unique<BindStream>();
    bind_stream_->SetBind(stream);
  }

  void SetVendorName(std::string_view vendor_name) {
    vendor_name_ = std::string(vendor_name);
  }
  void SetBatchSizeHint(int64_t batch_size_hint_bytes) {
    batch_size_hint_bytes_ = batch_size_hint_bytes;
  }
  bool streaming_rows() const { return streaming_rows_; }
  // True while the server still owes this reader a packet.
  bool server_streaming() const { return helper_.streaming(); }
  // Drop a server read so the connection can run another command. A result
  // that is already fully buffered stays readable.
  void ReleaseConnection();
  // Finish an open read on this thread. Safe to call more than once.
  void Close();
  // Thread-safe. Sets the cancel flag. The reading thread sends the cancel.
  AdbcStatusCode RequestCancel(struct AdbcError* error);
  void SetConnection(std::shared_ptr<PostgresConnection> connection) {
    connection_ = std::move(connection);
  }

  int GetSchema(struct ArrowSchema* out);
  int GetNext(struct ArrowArray* out);
  const char* GetLastError();
  static const struct AdbcError* ErrorFromArrayStream(struct ArrowArrayStream* stream,
                                                      AdbcStatusCode* status);

  Status ToArrayStream(int64_t* affected_rows, struct ArrowArrayStream* out);

  Status Initialize(int64_t* affected_rows);

 protected:
  std::shared_ptr<PostgresConnection> connection_;
  PGconn* conn_;
  PqResultHelper helper_;
  std::unique_ptr<BindStream> bind_stream_;
  std::shared_ptr<PostgresTypeResolver> type_resolver_;
  std::vector<std::unique_ptr<PostgresCopyFieldReader>> field_readers_;
  nanoarrow::UniqueSchema schema_;
  bool autocommit_;
  std::string vendor_name_;
  int64_t batch_size_hint_bytes_ = 16777216;
  int next_row_ = 0;
  bool streaming_rows_ = false;
  bool closed_ = false;
  // Written by the cancel thread, read by the reading thread.
  std::atomic<bool> cancel_requested_{false};
  // Set when this reader issued BEGIN so a multi-statement query stays one
  // transaction. Cleared once that transaction is committed or rolled back.
  bool own_txn_ = false;
  // A client-side read error. Close rolls the owned script back.
  bool script_failed_ = false;
  // The streamed statement of an owned script is inside SAVEPOINT adbc_stream.
  bool savepoint_ = false;
  AdbcStatusCode status_ = ADBC_STATUS_OK;
  struct AdbcError error_;
  struct ArrowError na_error_;

  explicit PqResultArrayReader(PqResultArrayReader* other)
      : connection_(std::move(other->connection_)),
        conn_(other->conn_),
        helper_(std::move(other->helper_)),
        bind_stream_(std::move(other->bind_stream_)),
        type_resolver_(std::move(other->type_resolver_)),
        field_readers_(std::move(other->field_readers_)),
        schema_(std::move(other->schema_)),
        batch_size_hint_bytes_(other->batch_size_hint_bytes_),
        next_row_(other->next_row_),
        streaming_rows_(other->streaming_rows_),
        closed_(other->closed_),
        cancel_requested_(other->cancel_requested_.load()),
        own_txn_(other->own_txn_),
        script_failed_(other->script_failed_),
        savepoint_(other->savepoint_),
        status_(other->status_) {
    other->cancel_requested_.store(false);
    other->own_txn_ = false;
    other->script_failed_ = false;
    other->savepoint_ = false;
    helper_.SetCancelFlag(&cancel_requested_);
    ArrowErrorInit(&na_error_);
    error_ = ADBC_ERROR_INIT;
  }

  Status BindNextAndExecute(int64_t* affected_rows);
  virtual Status ExecuteAll(int64_t* affected_rows);
  Status OpenRowStream();
  // A command tag is success. A subclass may also accept a tuples result.
  virtual bool CommandSucceeded(const PGresult* result) const;
  // When true, the last statement of an owned script is wrapped in a savepoint
  // so closing the stream can cancel it. A server that rejects SAVEPOINT
  // returns false and the close drains that statement instead.
  virtual bool ReleaseCanUseSavepoint() const { return true; }
  // End the transaction this reader opened. commit is ignored when the server
  // has already aborted it. Does nothing when this reader did not begin one.
  Status EndOwnedTransaction(bool commit);
  int GetNextStream(struct ArrowArray* out);
  int Fail(const Status& status);
  int FailArrow(int code);
  // Record a client-side read failure so Close rolls the script back.
  int FailRead(int code);

  // Prepare the query with the bind stream's parameter types
  virtual Status PrepareWithParamTypes();
  // Resolve the types of any all-null (Arrow NA) parameters from the types the
  // server expects
  virtual Status ResolveNullParamTypes();
  // Reconcile the bind stream with the prepared statement's parameter types
  Status ReconcileWithPreparedParamTypes();
  // The name to give a result column the server named name
  virtual std::string ResultColumnName(std::string name) const { return name; }

  void ResetErrors() {
    ArrowErrorInit(&na_error_);

    if (error_.private_data != nullptr) {
      error_.release(&error_);
    }
    error_ = ADBC_ERROR_INIT;
  }
};

}  // namespace adbcpq
