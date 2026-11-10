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

#include "result_helper.h"

#include <charconv>
#include <memory>
#include <string>
#include <vector>

#include "driver/framework/status.h"
#include "error.h"

namespace adbcpq {

PqResultHelper::~PqResultHelper() {
  PQclear(pending_);
  pending_ = nullptr;
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
  char errbuf[256];
  PGcancel* cancel = PQgetCancel(conn);
  if (cancel == nullptr) return false;
  const int rc = PQcancel(cancel, errbuf, sizeof(errbuf));
  PQfreeCancel(cancel);
  return rc == 1;
#endif
}

}  // namespace

void PqResultHelper::AbandonStream() {
  if (!streaming_ && pending_ == nullptr) return;
  streaming_ = false;
  PQclear(pending_);
  pending_ = nullptr;
  // Cancel only when the next read would block. If the rest is already
  // buffered, a cancel has nothing to interrupt and can hit the next query.
  bool sent_cancel = false;
  if (PQconsumeInput(conn_) == 1 && PQisBusy(conn_) == 1) {
    sent_cancel = SendCancel(conn_);
  }
  PGresult* result;
  while ((result = PQgetResult(conn_)) != nullptr) {
    PQclear(result);
  }
  // If the cancel missed this query, spend it on a dummy command.
  if (sent_cancel && PQstatus(conn_) == CONNECTION_OK &&
      PQtransactionStatus(conn_) != PQTRANS_INERROR) {
    PGresult* probe = PQexec(conn_, "SELECT 1");
    PQclear(probe);
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

  // libpq-fe.h defines LIBPQ_HAS_CHUNK_MODE on libpq 17+. Older headers have
  // no chunk API; single-row mode is their only batching.
  int armed;
#ifdef LIBPQ_HAS_CHUNK_MODE
  constexpr int kStreamChunkRows = 1024;
  armed = PQsetChunkedRowsMode(conn_, kStreamChunkRows);
#else
  armed = PQsetSingleRowMode(conn_);
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
  if (!streaming_ || pending_ != nullptr) return;
  if (PQconsumeInput(conn_) != 1 || PQisBusy(conn_) != 0) return;
  PGresult* next = PQgetResult(conn_);
  if (next == nullptr) {
    streaming_ = false;
    return;
  }
  const ExecStatusType status = PQresultStatus(next);
  if (IsRowPacket(status)) {
    pending_ = next;
    return;
  }
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK &&
      status != PGRES_EMPTY_QUERY) {
    // An error has to reach the next read. Don't commit over it.
    pending_ = next;
    return;
  }
  PQclear(next);
  PGresult* extra;
  while ((extra = PQgetResult(conn_)) != nullptr) {
    PQclear(extra);
  }
  streaming_ = false;
}

Status PqResultHelper::NextPacket() {
  if (!streaming_ && pending_ == nullptr) {
    ClearResult();
    return Status::Ok();
  }

  ClearResult();
  if (pending_ != nullptr) {
    result_ = pending_;
    pending_ = nullptr;
  } else {
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
                             PQerrorMessage(conn_), query_.c_str());
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
        PQerrorMessage(conn_), query_.c_str());
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
    return MakeStatus(result_, "[libpq] Failed to execute query '{}': {}", query_.c_str(),
                      PQerrorMessage(conn_));
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
        PQerrorMessage(conn_), copy_query.c_str());
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
