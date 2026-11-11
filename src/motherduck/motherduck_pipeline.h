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

// Pipelined execution for MotherDuck: multi-statement queries run as separate
// commands, and bound rows are sent without waiting on each in turn.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <libpq-fe.h>

#include "bind_stream.h"
#include "driver/framework/status.h"
#include "result_helper.h"

namespace adbcpq {

// Split a query on the semicolons between its statements, skipping those inside
// string literals, quoted identifiers, dollar quotes and comments. Statements
// with nothing but whitespace or comments are dropped. backslash_escapes says
// whether backslashes escape quotes in every literal, as they do when
// standard_conforming_strings is off, rather than only in E'' literals.
std::vector<std::string> SplitStatements(std::string_view query,
                                         bool backslash_escapes = false);

// Whether a statement is a COPY, ignoring leading whitespace and comments.
bool IsCopyStatement(std::string_view statement);

// Rewrite each parameter $n outside quotes and comments to
// CAST($n AS <types[n - 1]>), leaving it as is if that type is empty or
// missing. backslash_escapes is as for SplitStatements.
std::string CastParameters(std::string_view query, const std::vector<std::string>& types,
                           bool backslash_escapes = false);

// Undo CastParameters in a result column name the server derived from the
// cast query: each CAST($n AS ...) for a parameter types gives a type to
// becomes $n again.
std::string UncastParameters(std::string_view name,
                             const std::vector<std::string>& types);

// Whether every string literal on this connection takes backslash escapes
// (standard_conforming_strings is off).
bool BackslashEscapes(PGconn* conn);

// Whether a statement that isn't a COPY succeeded. Without
// return_pg_command_tags, MotherDuck answers DDL with an empty result set (a
// Count or Success column) instead of a command tag, so PGRES_TUPLES_OK counts
// as success alongside PGRES_COMMAND_OK.
inline bool IsResultOk(const PGresult* result) {
  ExecStatusType status = PQresultStatus(result);
  return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

// Run commands through one libpq pipeline ending in a single sync, sending and
// reading at the same time so neither side waits on the other. send_next
// queues the next command (e.g. with PQsendQueryParams) and sets *sent, or
// leaves it false once there are none left. on_result takes ownership of each
// result other than the sync, along with the index of its command. At most
// max_in_flight commands go unanswered at a time.
// As with a multi-statement query, the commands share one implicit
// transaction: after a command fails the server skips the rest, so sending
// stops; if send_next fails, a failing command is sent so the commands
// already run aren't committed either. Commands' own errors only reach
// on_result; the returned status covers send_next's errors and failures to
// send, receive, or run the pipeline. A COPY is ended and reported as not
// supported; a caller that may run one passes max_in_flight = 1, since ending
// it lets the server run whatever is queued behind it.
adbc::driver::Status RunPipeline(
    PGconn* conn, const std::function<adbc::driver::Status(bool* sent)>& send_next,
    const std::function<void(size_t, PGresult*)>& on_result,
    size_t max_in_flight = SIZE_MAX);

// Like helper->Execute(), but run a query of several statements as separate
// commands in one pipeline, leaving the last result (or the first error) in
// helper. The endpoint binds every statement of a multi-statement simple query
// before running any of them, so later statements can't see objects created by
// earlier ones. query must be the query helper was created with.
adbc::driver::Status ExecuteScript(PGconn* conn, const std::string& query,
                                   PqResultHelper* helper);

// Queue the bind stream's current row for the prepared statement in a
// pipeline. libpq copies the parameters, so the next row can reuse the buffer.
adbc::driver::Status SendCurrentRow(BindStream* stream, PGconn* conn, int result_format);

}  // namespace adbcpq
