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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "driver/framework/status.h"
#include "error.h"

namespace adbcpq {

PqResultHelper::~PqResultHelper() { ClearResult(); }

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

  if (!IsResultOk(result_)) {
    return MakeStatus(result_, "[libpq] Failed to execute query '{}': {}", query_.c_str(),
                      PQerrorMessage(conn_));
  }

  return Status::Ok();
}

Status PqResultHelper::ExecuteScript() {
  // With standard_conforming_strings off, backslashes escape quotes in every
  // string literal, not just E'' ones.
  const char* conforming = PQparameterStatus(conn_, "standard_conforming_strings");
  const bool backslash_escapes =
      conforming != nullptr && std::strcmp(conforming, "off") == 0;
  std::vector<std::string> statements = SplitStatements(query_, backslash_escapes);
  if (statements.size() <= 1) {
    return Execute();
  }
  // Ending a COPY makes libpq end the failed transaction early, so nothing may
  // be queued behind one: run batches with a COPY one statement at a time.
  const bool has_copy =
      std::any_of(statements.begin(), statements.end(), IsCopyStatement);
  return ExecuteBatch(statements, has_copy ? 1 : SIZE_MAX);
}

Status PqResultHelper::ExecuteBatch(const std::vector<std::string>& statements,
                                    size_t max_in_flight) {
  ClearResult();
  size_t next = 0;
  Status status;
  Status pipeline_status = RunPipeline(
      conn_,
      [&](bool* sent) -> Status {
        *sent = next < statements.size();
        if (!*sent) return Status::Ok();
        if (PQsendQueryParams(conn_, statements[next].c_str(), /*nParams=*/0,
                              /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                              /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                              static_cast<int>(Format::kText)) != 1) {
          return MakeStatus(nullptr, "[libpq] Failed to send query '{}': {}",
                            statements[next], PQerrorMessage(conn_));
        }
        next++;
        return Status::Ok();
      },
      [&](size_t index, PGresult* result) {
        KeepPipelineResult(result, statements[index], &status);
      },
      max_in_flight);
  // A statement's own error explains more than the pipeline failing after it.
  if (!status.ok()) return status;
  return pipeline_status;
}

void PqResultHelper::KeepPipelineResult(PGresult* result, const std::string& statement,
                                        Status* status) {
  switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
    case PGRES_COMMAND_OK:
    case PGRES_EMPTY_QUERY:
      // Keep the last result, as PQexec would.
      if (status->ok()) {
        SetResult(result);
      } else {
        PQclear(result);
      }
      break;
    case PGRES_PIPELINE_ABORTED:
      // Skipped because an earlier statement failed
      PQclear(result);
      break;
    default:
      if (status->ok()) {
        SetResult(result);
        *status = MakeStatus(result_, "[libpq] Failed to execute query '{}': {}",
                             statement, PQresultErrorMessage(result_));
      } else {
        PQclear(result);
      }
      break;
  }
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

namespace {

// How many commands to queue before asking the server for results and
// checking what has come back, which bounds how long a failed command goes
// unnoticed. A group also ends early once the socket stops taking data.
constexpr size_t kCommandsPerFlush = 64;

// The longest to wait on the socket before checking libpq again. With SSL,
// libpq can be left holding decrypted data the socket no longer shows as
// readable, so a wait mustn't be unbounded.
constexpr int kWaitMillis = 100;

// A command that fails to parse on any server, sent to fail the pipeline.
constexpr const char* kAbortCommand = "ADBC_ABORT_PIPELINE";

// Wait until the connection's socket is readable, or writable as well if
// `write` is set, or kWaitMillis passes.
Status WaitForSocket(PGconn* conn, bool write) {
  const int socket = PQsocket(conn);
  if (socket < 0) {
    return Status::IO("[libpq] Connection has no socket: ", PQerrorMessage(conn));
  }
#ifdef _WIN32
  WSAPOLLFD fd = {};
  fd.fd = static_cast<SOCKET>(socket);
  fd.events = static_cast<SHORT>(POLLRDNORM | (write ? POLLWRNORM : 0));
  const int ready = WSAPoll(&fd, 1, kWaitMillis);
#else
  struct pollfd fd = {};
  fd.fd = socket;
  fd.events = static_cast<short>(POLLIN | (write ? POLLOUT : 0));
  int ready;
  do {
    ready = poll(&fd, 1, kWaitMillis);
  } while (ready < 0 && errno == EINTR);
#endif
  if (ready < 0) {
    return Status::IO("[libpq] Failed to wait on the connection");
  }
  return Status::Ok();
}

// End a COPY started by a pipelined command, which has no data to send or
// anywhere to put it. Returns whether that took PQputCopyEnd, which makes
// libpq send a Sync of its own.
bool EndCopy(PGconn* conn, ExecStatusType copy_status) {
  // Move the COPY data in blocking mode so it can't stop half done.
  PQsetnonblocking(conn, 0);
  const bool ended_copy_in = copy_status != PGRES_COPY_OUT;
  if (ended_copy_in) {
    PQputCopyEnd(conn, "COPY is not supported in a pipeline");
  }
  if (copy_status != PGRES_COPY_IN) {
    char* buffer = nullptr;
    while (PQgetCopyData(conn, &buffer, /*async=*/0) >= 0) {
      PQfreemem(buffer);
    }
  }
  PQsetnonblocking(conn, 1);
  return ended_copy_in;
}

// PQputCopyEnd's own Sync makes the server send one more ReadyForQuery than
// libpq expects, and libpq would hand it to the next query as that query's
// result. libpq reports such a reply as an extra sync when a command is
// queued behind it, so queue a no-op and a sync and read through both. The
// connection must be blocking.
void AbsorbStraySync(PGconn* conn) {
  if (PQsendQueryParams(conn, "SELECT 1", /*nParams=*/0, /*paramTypes=*/nullptr,
                        /*paramValues=*/nullptr, /*paramLengths=*/nullptr,
                        /*paramFormats=*/nullptr, /*resultFormat=*/0) != 1 ||
      PQpipelineSync(conn) != 1) {
    return;
  }
  bool saw_query_result = false;
  while (true) {
    PGresult* result = PQgetResult(conn);
    if (result == nullptr) {
      // A null follows the no-op's results; any other means the connection
      // broke.
      if (saw_query_result && PQstatus(conn) == CONNECTION_OK) continue;
      return;
    }
    const bool synced = PQresultStatus(result) == PGRES_PIPELINE_SYNC;
    PQclear(result);
    if (synced && saw_query_result) return;
    if (!synced) saw_query_result = true;
  }
}

}  // namespace

Status RunPipeline(PGconn* conn, const std::function<Status(bool* sent)>& send_next,
                   const std::function<void(size_t, PGresult*)>& on_result,
                   size_t max_in_flight) {
  if (PQenterPipelineMode(conn) != 1) {
    return MakeStatus(nullptr, "[libpq] Failed to enter pipeline mode: {}",
                      PQerrorMessage(conn));
  }
  // Without blocking, we can read results while commands are still going out.
  // Blocking writes deadlock once the server stops reading because it's
  // blocked writing results we aren't reading.
  const int was_nonblocking = PQisnonblocking(conn);
  if (PQsetnonblocking(conn, 1) != 0) {
    Status status =
        MakeStatus(nullptr, "[libpq] Failed to make connection nonblocking: {}",
                   PQerrorMessage(conn));
    PQexitPipelineMode(conn);
    return status;
  }

  Status status;
  bool sending = true;         // commands may remain to be sent
  bool synced = false;         // the closing sync has been queued
  bool done = false;           // the sync's result has arrived
  bool ended_copy_in = false;  // libpq sent a Sync of its own
  size_t sent_count = 0;       // commands sent through send_next
  size_t index = 0;            // the command the next result belongs to
  bool in_command = false;     // a result for that command has arrived

  auto sync = [&]() {
    if (PQpipelineSync(conn) != 1) {
      if (status.ok()) {
        status = MakeStatus(nullptr, "[libpq] Failed to sync pipeline: {}",
                            PQerrorMessage(conn));
      }
      return false;
    }
    synced = true;
    return true;
  };

  while (!done) {
    const int flushed = PQflush(conn);
    if (flushed < 0) {
      status = MakeStatus(nullptr, "[libpq] Failed to send pipeline: {}",
                          PQerrorMessage(conn));
      break;
    }

    // Queue more only once everything before has gone out, so a slow server
    // can't make us buffer the whole batch in memory.
    bool queued = false;
    if (flushed == 0 && !synced) {
      size_t group = 0;
      while (sending && group < kCommandsPerFlush && sent_count - index < max_in_flight) {
        bool sent = false;
        Status send_status = send_next(&sent);
        if (!send_status.ok()) {
          status = std::move(send_status);
          // At the sync the server would commit what has already run, as it
          // wouldn't if a command had failed. Fail one so it doesn't.
          if (sent_count > 0) {
            PQsendQueryParams(conn, kAbortCommand, /*nParams=*/0,
                              /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                              /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                              /*resultFormat=*/0);
          }
          sent = false;
        }
        if (!sent) {
          sending = false;
          break;
        }
        sent_count++;
        group++;
        // Stop queueing once the socket can't take more: commands can be
        // large (a batch of ingested rows), and libpq would otherwise hold a
        // whole group of them in memory.
        if (PQflush(conn) != 0) break;
      }
      if (!sending) {
        if (!sync()) break;
        queued = true;
      } else if (group > 0) {
        // Without a sync, the server holds results back until asked.
        if (PQsendFlushRequest(conn) != 1) {
          status = MakeStatus(nullptr, "[libpq] Failed to send pipeline: {}",
                              PQerrorMessage(conn));
          break;
        }
        queued = true;
      }
    }

    // Read whatever has arrived, without waiting, so a failed command stops
    // the rest from being sent.
    if (PQconsumeInput(conn) != 1) {
      status = MakeStatus(nullptr, "[libpq] Failed to read pipeline results: {}",
                          PQerrorMessage(conn));
      break;
    }
    bool progressed = false;
    while (!done && !PQisBusy(conn)) {
      PGresult* result = PQgetResult(conn);
      if (result == nullptr) {
        // libpq returns null after each command's results, and also when
        // nothing more has arrived yet.
        if (!in_command) break;
        index++;
        in_command = false;
        progressed = true;
        continue;
      }

      progressed = true;
      const ExecStatusType result_status = PQresultStatus(result);
      if (result_status == PGRES_PIPELINE_SYNC) {
        PQclear(result);
        done = true;
        break;
      }

      in_command = true;
      if (result_status == PGRES_COPY_IN || result_status == PGRES_COPY_OUT ||
          result_status == PGRES_COPY_BOTH) {
        PQclear(result);
        if (status.ok()) {
          status = Status(ADBC_STATUS_NOT_IMPLEMENTED,
                          "[libpq] COPY is not supported in a pipeline");
        }
        sending = false;
        if (EndCopy(conn, result_status)) {
          ended_copy_in = true;
          // Sync before reading on, so the reply to libpq's Sync is taken
          // as ours rather than arriving with nothing to match.
          if (!synced && !sync()) break;
        }
        continue;
      }

      // The server skips everything up to the sync after a failure.
      if (result_status == PGRES_FATAL_ERROR) sending = false;
      if (index < sent_count) {
        on_result(index, result);
      } else {
        PQclear(result);  // the abort command's failure
      }
    }
    if (done || progressed || queued) continue;
    if (!status.ok() && !synced) break;  // a failed sync

    Status wait_status = WaitForSocket(conn, /*write=*/flushed == 1);
    if (!wait_status.ok()) {
      status = std::move(wait_status);
      break;
    }
  }

  if (PQsetnonblocking(conn, 0) != 0 && status.ok()) {
    status = MakeStatus(nullptr, "[libpq] Failed to restore blocking mode: {}",
                        PQerrorMessage(conn));
  }
  if (done && ended_copy_in) AbsorbStraySync(conn);
  // Exiting only fails if results are still pending, i.e. we gave up on the
  // connection above, in which case the earlier error is the useful one.
  if (PQexitPipelineMode(conn) != 1 && status.ok()) {
    status = MakeStatus(nullptr, "[libpq] Failed to exit pipeline mode: {}",
                        PQerrorMessage(conn));
  }
  if (was_nonblocking) PQsetnonblocking(conn, 1);
  return status;
}

namespace {

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// If a comment starts at query[start], return the index just past it;
// otherwise return start.
size_t SkipComment(std::string_view query, size_t start) {
  if (query.compare(start, 2, "--") == 0) {
    const size_t end = query.find('\n', start);
    return end == std::string_view::npos ? query.size() : end;
  }
  if (query.compare(start, 2, "/*") == 0) {
    // Block comments nest.
    int depth = 1;
    size_t i = start + 2;
    while (i < query.size() && depth > 0) {
      if (query.compare(i, 2, "/*") == 0) {
        depth++;
        i += 2;
      } else if (query.compare(i, 2, "*/") == 0) {
        depth--;
        i += 2;
      } else {
        i++;
      }
    }
    return i;
  }
  return start;
}

// Return the index just past the literal or identifier quoted by query[start].
size_t SkipQuoted(std::string_view query, size_t start, bool backslash_escapes) {
  const char quote = query[start];
  size_t i = start + 1;
  while (i < query.size()) {
    if (backslash_escapes && query[i] == '\\') {
      i += 2;
    } else if (query[i] == quote) {
      // A doubled quote stands for itself.
      if (i + 1 < query.size() && query[i + 1] == quote) {
        i += 2;
      } else {
        return i + 1;
      }
    } else {
      i++;
    }
  }
  return query.size();
}

// Return the "$tag$" opening a dollar quote at query[start], or an empty view
// if there isn't one (e.g. a $1 parameter or a $ inside an identifier).
std::string_view DollarQuoteTag(std::string_view query, size_t start) {
  if (query[start] != '$') return {};
  if (start > 0 && IsIdentifierChar(query[start - 1])) return {};
  size_t i = start + 1;
  if (i < query.size() && std::isdigit(static_cast<unsigned char>(query[i]))) return {};
  while (i < query.size() && IsIdentifierChar(query[i])) i++;
  if (i < query.size() && query[i] == '$') return query.substr(start, i - start + 1);
  return {};
}

std::string_view TrimWhitespace(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
  return text.substr(begin, end - begin);
}

}  // namespace

std::vector<std::string> SplitStatements(std::string_view query, bool backslash_escapes) {
  std::vector<std::string> statements;
  size_t start = 0;
  // Whether the current statement has anything besides whitespace and comments
  bool has_content = false;

  auto finish_statement = [&](size_t end) {
    if (has_content) {
      statements.emplace_back(TrimWhitespace(query.substr(start, end - start)));
    }
    start = end + 1;
    has_content = false;
  };

  size_t i = 0;
  while (i < query.size()) {
    const char c = query[i];
    if (const size_t end = SkipComment(query, i); end != i) {
      i = end;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
      continue;
    }
    if (c == ';') {
      finish_statement(i);
      i++;
      continue;
    }

    has_content = true;
    if (c == '\'') {
      // E'...' strings take backslash escapes.
      const bool escape_string = i > 0 && (query[i - 1] == 'E' || query[i - 1] == 'e') &&
                                 !(i > 1 && IsIdentifierChar(query[i - 2]));
      i = SkipQuoted(query, i, backslash_escapes || escape_string);
    } else if (c == '"') {
      i = SkipQuoted(query, i, /*backslash_escapes=*/false);
    } else if (std::string_view tag = DollarQuoteTag(query, i); !tag.empty()) {
      size_t close = query.find(tag, i + tag.size());
      i = close == std::string_view::npos ? query.size() : close + tag.size();
    } else {
      i++;
    }
  }
  finish_statement(query.size());

  return statements;
}

bool IsCopyStatement(std::string_view statement) {
  size_t i = 0;
  while (i < statement.size()) {
    if (std::isspace(static_cast<unsigned char>(statement[i]))) {
      i++;
    } else if (const size_t end = SkipComment(statement, i); end != i) {
      i = end;
    } else {
      break;
    }
  }

  constexpr std::string_view kCopy = "copy";
  if (statement.size() - i < kCopy.size()) return false;
  for (size_t j = 0; j < kCopy.size(); j++) {
    if (std::tolower(static_cast<unsigned char>(statement[i + j])) != kCopy[j]) {
      return false;
    }
  }
  i += kCopy.size();
  return i == statement.size() || !IsIdentifierChar(statement[i]);
}

}  // namespace adbcpq
