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
#else
#include <poll.h>
#endif

#include "motherduck_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver/framework/status.h"
#include "error.h"

using adbc::driver::Status;

namespace adbcpq {

namespace {

// How many commands to queue before asking the server for results and
// checking what has come back. libpq writes its buffer out in 8 KB chunks as
// it fills, so this mostly bounds how long a failed command goes unnoticed.
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
    return Status::IO("[libpq] Connection has no socket: ", PqErrorText(nullptr, conn));
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
                      PqErrorText(nullptr, conn));
  }
  // Without blocking, we can read results while commands are still going out.
  // Blocking writes deadlock once the server stops reading because it's
  // blocked writing results we aren't reading.
  const int was_nonblocking = PQisnonblocking(conn);
  if (PQsetnonblocking(conn, 1) != 0) {
    Status status =
        MakeStatus(nullptr, "[libpq] Failed to make connection nonblocking: {}",
                   PqErrorText(nullptr, conn));
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
                            PqErrorText(nullptr, conn));
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
                          PqErrorText(nullptr, conn));
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
      }
      if (!sending) {
        if (!sync()) break;
        queued = true;
      } else if (group > 0) {
        // Without a sync, the server holds results back until asked.
        if (PQsendFlushRequest(conn) != 1) {
          status = MakeStatus(nullptr, "[libpq] Failed to send pipeline: {}",
                              PqErrorText(nullptr, conn));
          break;
        }
        queued = true;
      }
    }

    // Read whatever has arrived, without waiting, so a failed command stops
    // the rest from being sent.
    if (PQconsumeInput(conn) != 1) {
      status = MakeStatus(nullptr, "[libpq] Failed to read pipeline results: {}",
                          PqErrorText(nullptr, conn));
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
                        PqErrorText(nullptr, conn));
  }
  if (done && ended_copy_in) AbsorbStraySync(conn);
  // Exiting only fails if results are still pending, i.e. we gave up on the
  // connection above, in which case the earlier error is the useful one.
  if (PQexitPipelineMode(conn) != 1 && status.ok()) {
    status = MakeStatus(nullptr, "[libpq] Failed to exit pipeline mode: {}",
                        PqErrorText(nullptr, conn));
  }
  if (was_nonblocking) PQsetnonblocking(conn, 1);
  return status;
}

namespace {

// Keep a statement's result in helper if it's the last so far or the first
// error.
void KeepPipelineResult(PqResultHelper* helper, PGresult* result,
                        const std::string& statement, Status* status) {
  switch (PQresultStatus(result)) {
    case PGRES_TUPLES_OK:
    case PGRES_COMMAND_OK:
    case PGRES_EMPTY_QUERY:
      // Keep the last result, as PQexec would.
      if (status->ok()) {
        helper->SetResult(result);
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
        helper->SetResult(result);
        *status = MakeStatus(result, "[libpq] Failed to execute query '{}': {}",
                             RedactQuery(statement), PqErrorText(result, nullptr));
      } else {
        PQclear(result);
      }
      break;
  }
}

// Run statements in one pipeline, with at most max_in_flight unanswered.
Status ExecuteBatch(PGconn* conn, const std::vector<std::string>& statements,
                    size_t max_in_flight, PqResultHelper* helper) {
  helper->ClearResult();
  size_t next = 0;
  Status status;
  Status pipeline_status = RunPipeline(
      conn,
      [&](bool* sent) -> Status {
        *sent = next < statements.size();
        if (!*sent) return Status::Ok();
        if (PQsendQueryParams(conn, statements[next].c_str(), /*nParams=*/0,
                              /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                              /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                              static_cast<int>(PqResultHelper::Format::kText)) != 1) {
          return MakeStatus(nullptr, "[libpq] Failed to send query '{}': {}",
                            RedactQuery(statements[next]), PqErrorText(nullptr, conn));
        }
        next++;
        return Status::Ok();
      },
      [&](size_t index, PGresult* result) {
        KeepPipelineResult(helper, result, statements[index], &status);
      },
      max_in_flight);
  // A statement's own error explains more than the pipeline failing after it.
  if (!status.ok()) return status;
  return pipeline_status;
}

}  // namespace

Status ExecuteScript(PGconn* conn, const std::string& query, PqResultHelper* helper) {
  std::vector<std::string> statements = SplitStatements(query, BackslashEscapes(conn));
  if (statements.size() <= 1) {
    return helper->Execute();
  }
  // Ending a COPY makes libpq end the failed transaction early, so nothing may
  // be queued behind one: run batches with a COPY one statement at a time.
  const bool has_copy =
      std::any_of(statements.begin(), statements.end(), IsCopyStatement);
  return ExecuteBatch(conn, statements, has_copy ? 1 : SIZE_MAX, helper);
}

Status SendCurrentRow(BindStream* stream, PGconn* conn, int result_format) {
  UNWRAP_STATUS(stream->SerializeCurrentRow());

  if (PQsendQueryPrepared(conn, /*stmtName=*/"",
                          /*nParams=*/stream->bind_schema->n_children,
                          stream->param_values.data(), stream->param_lengths.data(),
                          stream->param_formats.data(), result_format) != 1) {
    return MakeStatus(nullptr, "[libpq] Failed to send prepared statement: {}",
                      PqErrorText(nullptr, conn));
  }
  return Status::Ok();
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

// If a string literal, quoted identifier or dollar quote starts at query[start],
// return the index just past it; otherwise return start.
size_t SkipLiteral(std::string_view query, size_t start, bool backslash_escapes) {
  const char c = query[start];
  if (c == '\'') {
    // E'...' strings take backslash escapes.
    const bool escape_string = start > 0 &&
                               (query[start - 1] == 'E' || query[start - 1] == 'e') &&
                               !(start > 1 && IsIdentifierChar(query[start - 2]));
    return SkipQuoted(query, start, backslash_escapes || escape_string);
  }
  if (c == '"') {
    return SkipQuoted(query, start, /*backslash_escapes=*/false);
  }
  if (std::string_view tag = DollarQuoteTag(query, start); !tag.empty()) {
    size_t close = query.find(tag, start + tag.size());
    return close == std::string_view::npos ? query.size() : close + tag.size();
  }
  return start;
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
    if (const size_t end = SkipLiteral(query, i, backslash_escapes); end != i) {
      i = end;
    } else {
      i++;
    }
  }
  finish_statement(query.size());

  return statements;
}

std::string CastParameters(std::string_view query, const std::vector<std::string>& types,
                           bool backslash_escapes) {
  std::string out;
  size_t copied = 0;  // query[copied, i) has yet to be copied to out
  size_t i = 0;
  while (i < query.size()) {
    if (const size_t end = SkipComment(query, i); end != i) {
      i = end;
    } else if (const size_t end = SkipLiteral(query, i, backslash_escapes); end != i) {
      i = end;
    } else if (query[i] == '$' && !(i > 0 && IsIdentifierChar(query[i - 1])) &&
               i + 1 < query.size() &&
               std::isdigit(static_cast<unsigned char>(query[i + 1]))) {
      size_t end = i + 1;
      size_t number = 0;
      while (end < query.size() && std::isdigit(static_cast<unsigned char>(query[end]))) {
        // Saturate rather than overflow; no query has that many parameters.
        number =
            number > (SIZE_MAX - 9) / 10 ? SIZE_MAX : number * 10 + (query[end] - '0');
        end++;
      }
      if (number >= 1 && number <= types.size() && !types[number - 1].empty()) {
        out.append(query.substr(copied, i - copied));
        out += "CAST(";
        out.append(query.substr(i, end - i));
        out += " AS ";
        out += types[number - 1];
        out += ")";
        copied = end;
      }
      i = end;
    } else {
      i++;
    }
  }
  out.append(query.substr(copied));
  return out;
}

std::string UncastParameters(std::string_view name,
                             const std::vector<std::string>& types) {
  static constexpr std::string_view kCast = "CAST($";
  static constexpr std::string_view kAs = " AS ";
  std::string out;
  size_t copied = 0;  // name[copied, i) has yet to be copied to out
  size_t i = 0;
  while ((i = name.find(kCast, i)) != std::string_view::npos) {
    const size_t param = i + kCast.size() - 1;  // the $
    size_t end = param + 1;
    size_t number = 0;
    while (end < name.size() && std::isdigit(static_cast<unsigned char>(name[end]))) {
      number = number > (SIZE_MAX - 9) / 10 ? SIZE_MAX : number * 10 + (name[end] - '0');
      end++;
    }
    if (end == param + 1 || number < 1 || number > types.size() ||
        types[number - 1].empty() || name.substr(end, kAs.size()) != kAs) {
      i++;
      continue;
    }
    // Find the cast's closing parenthesis; the type may have its own.
    int depth = 1;
    size_t close = end + kAs.size();
    for (; close < name.size() && depth > 0; close++) {
      if (name[close] == '(') depth++;
      if (name[close] == ')') depth--;
    }
    if (depth != 0) break;
    out.append(name.substr(copied, i - copied));
    out.append(name.substr(param, end - param));
    copied = i = close;
  }
  out.append(name.substr(copied));
  return out;
}

bool BackslashEscapes(PGconn* conn) {
  const char* conforming = PQparameterStatus(conn, "standard_conforming_strings");
  return conforming != nullptr && std::strcmp(conforming, "off") == 0;
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
