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
#else
#include <poll.h>
#endif

#include "result_helper.h"

#include <charconv>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "driver/framework/status.h"
#include "error.h"

namespace adbcpq {

PqResultHelper::~PqResultHelper() {
  for (PGresult* result : pending_) PQclear(result);
  pending_.clear();
  ClearResult();
  if (streaming_) AbandonStream();
}

namespace {

// Wait until the server has accepted the cancel. A bare PQcancel returns
// before the server applies it, so it can abort the next query instead.
bool SendCancel(PGconn* conn) {
#ifdef LIBPQ_HAS_ASYNC_CANCEL
  PGcancelConn* cancel = PQcancelCreate(conn);
  if (cancel == nullptr) return false;
  const int rc = PQcancelBlocking(cancel);
  PQcancelFinish(cancel);
  return rc == 1;
#else
  // libpq 15 and 16. The caller is blocked in this read and consumes the
  // result immediately, so the older asynchronous cancel still hits this query.
  char errbuf[256];
  PGcancel* cancel = PQgetCancel(conn);
  if (cancel == nullptr) return false;
  const int rc = PQcancel(cancel, errbuf, sizeof(errbuf));
  PQfreeCancel(cancel);
  return rc == 1;
#endif
}

bool IsFailure(const PGresult* result) {
  if (result == nullptr) return false;
  const ExecStatusType status = PQresultStatus(result);
  return status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE;
}

bool IsCancelError(const PGresult* result) {
  if (result == nullptr || PQresultStatus(result) != PGRES_FATAL_ERROR) return false;
  const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  return sqlstate != nullptr && std::strncmp(sqlstate, "57014", 5) == 0;
}

void WaitForSocket(int sock) {
  if (sock < 0) return;
#ifdef _WIN32
  fd_set input;
  FD_ZERO(&input);
  FD_SET(sock, &input);
  timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 50 * 1000;
  select(0, &input, nullptr, nullptr, &timeout);
#else
  pollfd pfd;
  pfd.fd = sock;
  pfd.events = POLLIN;
  pfd.revents = 0;
  poll(&pfd, 1, 50);
#endif
}

}  // namespace

void PqResultHelper::DiscardLocal() {
  for (PGresult* result : pending_) PQclear(result);
  pending_.clear();
  ClearResult();
  streaming_ = false;
}

void PqResultHelper::WaitUntilReady() {
  // The cancel thread only sets cancel_flag_. This thread sends the cancel,
  // and only while the next read would block, so it cannot hit a later query.
  for (;;) {
    if (PQconsumeInput(conn_) != 1) return;
    if (PQisBusy(conn_) == 0) return;
    const bool stop = stop_if_blocked_ || (cancel_flag_ != nullptr &&
                                           cancel_flag_->load(std::memory_order_acquire));
    if (stop && !cancel_sent_) {
      SendCancel(conn_);
      cancel_sent_ = true;
      return;
    }
    WaitForSocket(PQsocket(conn_));
  }
}

bool PqResultHelper::AbandonStream() {
  if (!streaming_ && pending_.empty()) return false;
  const auto failed_script = [this](const PGresult* result) {
    if (!IsFailure(result)) return false;
    // A cancel this drain sent, so the connection can be reused, is not a
    // failed script. A user cancel still is.
    if (IsCancelError(result) &&
        (cancel_flag_ == nullptr || !cancel_flag_->load(std::memory_order_acquire))) {
      return false;
    }
    return true;
  };
  bool failed = false;
  for (PGresult* result : pending_) {
    if (failed_script(result)) failed = true;
    PQclear(result);
  }
  pending_.clear();
  streaming_ = false;
  for (;;) {
    WaitUntilReady();
    PGresult* result = PQgetResult(conn_);
    if (result == nullptr) return failed;
    if (failed_script(result)) failed = true;
    PQclear(result);
  }
}

namespace {

bool IsRowPacket(ExecStatusType status) {
  if (status == PGRES_SINGLE_TUPLE) return true;
#ifdef LIBPQ_HAS_CHUNK_MODE
  if (status == PGRES_TUPLES_CHUNK) return true;
#endif
  return false;
}

}  // namespace

void AbortCopy(PGconn* conn, ExecStatusType status) {
  if (status == PGRES_COPY_IN || status == PGRES_COPY_BOTH) {
    PQputCopyEnd(conn, "COPY is not supported in a multi-statement query");
  }
  if (status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH) {
    // A finished COPY is already buffered. Cancelling then can hit the next command.
    if (PQconsumeInput(conn) == 1 && PQisBusy(conn) == 1) SendCancel(conn);
    char* buffer = nullptr;
    for (;;) {
      const int n = PQgetCopyData(conn, &buffer, /*async=*/0);
      if (buffer != nullptr) {
        PQfreemem(buffer);
        buffer = nullptr;
      }
      if (n < 0) break;
    }
  }
}

Status PqResultHelper::ExecuteCancellable(const std::string& sql) {
  if (PQsendQuery(conn_, sql.c_str()) != 1) {
    return MakeStatus(nullptr, "[libpq] Failed to execute query '{}': {}", sql.c_str(),
                      PQerrorMessage(conn_));
  }
  Status failure = Status::Ok();
  for (;;) {
    WaitUntilReady();
    PGresult* result = PQgetResult(conn_);
    if (result == nullptr) break;
    const ExecStatusType status = PQresultStatus(result);
    if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT ||
        status == PGRES_COPY_BOTH) {
      if (failure.ok()) {
        failure = Status(ADBC_STATUS_NOT_IMPLEMENTED,
                         "[libpq] COPY is not supported in a multi-statement query");
      }
      AbortCopy(conn_, status);
      PQclear(result);
      continue;
    }
    if (IsFailure(result) && failure.ok()) {
      if (cancel_flag_ != nullptr && cancel_flag_->load(std::memory_order_acquire)) {
        failure = Status(ADBC_STATUS_CANCELLED,
                         "[libpq] canceling statement due to user request");
        failure.SetSqlState("57014");
      } else {
        failure = MakeStatus(result, "[libpq] Failed to execute query '{}': {}",
                             sql.c_str(), PQresultErrorMessage(result));
      }
    }
    PQclear(result);
  }
  return failure;
}

Status PqResultHelper::BeginRowStream() {
  ClearResult();
  // One statement. PQsendQuery (simple protocol) is what accepts several
  // statements; the caller runs those before this.
  if (PQsendQueryParams(conn_, query_.c_str(), /*nParams=*/0, /*paramTypes=*/nullptr,
                        /*paramValues=*/nullptr, /*paramLengths=*/nullptr,
                        /*paramFormats=*/nullptr,
                        static_cast<int>(output_format_)) != 1) {
    return MakeStatus(nullptr, "[libpq] Failed to send query '{}': {}", query_.c_str(),
                      PQerrorMessage(conn_));
  }

#ifdef LIBPQ_HAS_CHUNK_MODE
  constexpr int kStreamChunkRows = 1024;
  const int armed = PQsetChunkedRowsMode(conn_, kStreamChunkRows);
#else
  // libpq 15 and 16 deliver one row per packet. PullBufferedTail still drains
  // every packet that is already buffered, so a finished script can commit.
  const int armed = PQsetSingleRowMode(conn_);
#endif
  if (armed != 1) {
    streaming_ = true;
    AbandonStream();
    return MakeStatus(nullptr, "[libpq] Failed to read query '{}' in batches: {}",
                      query_.c_str(), PQerrorMessage(conn_));
  }
  streaming_ = true;
  return Status::Ok();
}

void PqResultHelper::PullBufferedTail() {
  if (!streaming_) return;
  // Single-row mode (libpq 15 and 16) puts each row in its own packet. Keep
  // taking packets that are already buffered until the command completes, so
  // the script can commit without the caller reading every row.
  for (;;) {
    if (PQconsumeInput(conn_) != 1 || PQisBusy(conn_) != 0) return;
    PGresult* next = PQgetResult(conn_);
    if (next == nullptr) {
      streaming_ = false;
      return;
    }
    const ExecStatusType status = PQresultStatus(next);
    if (IsRowPacket(status)) {
      pending_.push_back(next);
      // One packet of read-ahead. More than that lets a fast server fill memory.
      return;
    }
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK &&
        status != PGRES_EMPTY_QUERY) {
      // An error has to reach the next read. Don't commit over it.
      pending_.push_back(next);
      return;
    }
    PQclear(next);
    PGresult* extra;
    while ((extra = PQgetResult(conn_)) != nullptr) {
      if (IsFailure(extra) || IsRowPacket(PQresultStatus(extra))) {
        pending_.push_back(extra);
        return;
      }
      PQclear(extra);
    }
    streaming_ = false;
    return;
  }
}

Status PqResultHelper::NextPacket() {
  if (!streaming_ && pending_.empty()) {
    ClearResult();
    return Status::Ok();
  }

  ClearResult();
  if (!pending_.empty()) {
    result_ = pending_.front();
    pending_.pop_front();
  } else {
    WaitUntilReady();
    result_ = PQgetResult(conn_);
  }
  if (result_ == nullptr) {
    streaming_ = false;
    if (PQstatus(conn_) == CONNECTION_BAD) {
      return Status::IO("[libpq] Failed to read query: ", PQerrorMessage(conn_));
    }
    return Status::Ok();
  }

  const ExecStatusType status = PQresultStatus(result_);
  if (IsRowPacket(status)) {
    PullBufferedTail();
    return Status::Ok();
  }

  if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK ||
      status == PGRES_EMPTY_QUERY) {
    PGresult* extra;
    while ((extra = PQgetResult(conn_)) != nullptr) {
      PQclear(extra);
    }
    streaming_ = false;
    return Status::Ok();
  }

  Status error = MakeStatus(result_, "[libpq] Failed to execute query '{}': {}",
                            query_.c_str(), PQresultErrorMessage(result_));
  ClearResult();
  PGresult* extra;
  while ((extra = PQgetResult(conn_)) != nullptr) {
    PQclear(extra);
  }
  streaming_ = false;
  return error;
}

Status PqResultHelper::PrepareInternal(int n_params, const Oid* param_oids) const {
  // TODO: make stmtName a unique identifier?
  PGresult* result =
      PQprepare(conn_, /*stmtName=*/"", query_.c_str(), n_params, param_oids);
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    auto status = MakeStatus(result, "Failed to prepare query: {}\nQuery was: {}",
                             PqErrorText(result, conn_), RedactQuery(query_));
    PQclear(result);
    return status;
  }

  PQclear(result);
  return Status::Ok();
}

Status PqResultHelper::Prepare() const { return PrepareInternal(0, nullptr); }

Status PqResultHelper::Prepare(const std::vector<Oid>& param_oids) const {
  return PrepareInternal(static_cast<int>(param_oids.size()), param_oids.data());
}

Status PqResultHelper::DescribePrepared() {
  ClearResult();
  result_ = PQdescribePrepared(conn_, /*stmtName=*/"");
  if (PQresultStatus(result_) != PGRES_COMMAND_OK) {
    Status status = MakeStatus(
        result_, "[libpq] Failed to describe prepared statement: {}\nQuery was: {}",
        PqErrorText(result_, conn_), RedactQuery(query_));
    ClearResult();
    return status;
  }

  return Status::Ok();
}

Status PqResultHelper::Execute(const std::vector<std::string>& params,
                               PostgresType* param_types) {
  if (params.size() == 0 && param_types == nullptr && output_format_ == Format::kText) {
    ClearResult();
    result_ = PQexec(conn_, query_.c_str());
  } else {
    std::vector<const char*> param_values;
    std::vector<int> param_lengths;
    std::vector<int> param_formats;

    for (const auto& param : params) {
      param_values.push_back(param.data());
      param_lengths.push_back(static_cast<int>(param.size()));
      param_formats.push_back(static_cast<int>(param_format_));
    }

    std::vector<Oid> param_oids;
    const Oid* param_oids_ptr = nullptr;
    if (param_types != nullptr) {
      param_oids.resize(params.size());
      for (size_t i = 0; i < params.size(); i++) {
        param_oids[i] = param_types->child(i).oid();
      }
      param_oids_ptr = param_oids.data();
    }

    ClearResult();
    result_ = PQexecParams(conn_, query_.c_str(), static_cast<int>(param_values.size()),
                           param_oids_ptr, param_values.data(), param_lengths.data(),
                           param_formats.data(), static_cast<int>(output_format_));
  }

  ExecStatusType status = PQresultStatus(result_);
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    return MakeStatus(result_, "[libpq] Failed to execute query '{}': {}",
                      RedactQuery(query_), PqErrorText(result_, conn_));
  }

  return Status::Ok();
}

Status PqResultHelper::ExecuteCopy() {
  // Remove trailing semicolon(s) from the query before feeding it into COPY
  while (!query_.empty() && query_.back() == ';') {
    query_.pop_back();
  }

  std::string copy_query = "COPY (" + query_ + ") TO STDOUT (FORMAT binary)";
  ClearResult();
  result_ = PQexecParams(conn_, copy_query.c_str(), /*nParams=*/0,
                         /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                         /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                         static_cast<int>(Format::kBinary));

  if (PQresultStatus(result_) != PGRES_COPY_OUT) {
    Status status = MakeStatus(
        result_,
        "[libpq] Failed to execute query: could not begin COPY: {}\nQuery was: {}",
        PqErrorText(result_, conn_), RedactQuery(copy_query));
    ClearResult();
    return status;
  }

  return Status::Ok();
}

Status PqResultHelper::ResolveParamTypes(PostgresTypeResolver& type_resolver,
                                         PostgresType* param_types) {
  struct ArrowError na_error;
  ArrowErrorInit(&na_error);

  const int num_params = PQnparams(result_);
  PostgresType root_type(PostgresTypeId::kRecord);

  for (int i = 0; i < num_params; i++) {
    const Oid pg_oid = PQparamtype(result_, i);
    PostgresType pg_type;
    if (pg_oid == 0) {
      // PostgreSQL didn't infer a type (can happen in ambiguous contexts).
      pg_type = PostgresType(PostgresTypeId::kUnknown).WithPgTypeInfo(0, "unknown");
    } else if (type_resolver.Find(pg_oid, &pg_type, &na_error) != NANOARROW_OK) {
      // Don't fail preparation just because we can't resolve an expected parameter
      // type. We'll fall back to Arrow-derived mapping.
      pg_type =
          PostgresType(PostgresTypeId::kUnknown)
              .WithPgTypeInfo(pg_oid, "unknown<oid:" + std::to_string(pg_oid) + ">");
    }

    std::string param_name = "$" + std::to_string(i + 1);
    root_type.AppendChild(param_name.c_str(), pg_type);
  }

  *param_types = root_type;
  return Status::Ok();
}

Status PqResultHelper::ResolveOutputTypes(PostgresTypeResolver& type_resolver,
                                          PostgresType* result_types) {
  struct ArrowError na_error;
  ArrowErrorInit(&na_error);

  const int num_fields = PQnfields(result_);
  PostgresType root_type(PostgresTypeId::kRecord);

  for (int i = 0; i < num_fields; i++) {
    const Oid pg_oid = PQftype(result_, i);
    PostgresType pg_type;
    if (type_resolver.Find(pg_oid, &pg_type, &na_error) != NANOARROW_OK) {
      // We couldn't look up the OID.
      // TODO(apache/arrow-adbc#1243): issue a warning (maybe reloading the
      // connection will load the OIDs if it was a newly created type)
      pg_type = PostgresType::Unnamed(pg_oid);
    }

    root_type.AppendChild(PQfname(result_, i),
                          pg_type.WithTypeModifier(PQfmod(result_, i)));
  }

  *result_types = root_type;
  return Status::Ok();
}

PGresult* PqResultHelper::ReleaseResult() {
  PGresult* out = result_;
  result_ = nullptr;
  return out;
}

int64_t PqResultHelper::AffectedRows() const {
  if (result_ == nullptr) {
    return -1;
  }

  char* first = PQcmdTuples(result_);
  char* last = first + strlen(first);
  if ((last - first) == 0) {
    return -1;
  }

  int64_t out;
  auto result = std::from_chars(first, last, out);

  if (result.ec == std::errc() && result.ptr == last) {
    return out;
  } else {
    return -1;
  }
}

}  // namespace adbcpq
