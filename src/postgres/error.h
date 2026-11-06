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

// Error handling utilities.

#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include <fmt/core.h>
#include <fmt/format.h>

#include "driver/framework/status.h"

using adbc::driver::Status;

namespace adbcpq {

struct DetailField {
  int code;
  std::string key;
};

static const std::vector<DetailField> kDetailFields = {
    {PG_DIAG_COLUMN_NAME, "PG_DIAG_COLUMN_NAME"},
    {PG_DIAG_CONTEXT, "PG_DIAG_CONTEXT"},
    {PG_DIAG_CONSTRAINT_NAME, "PG_DIAG_CONSTRAINT_NAME"},
    {PG_DIAG_DATATYPE_NAME, "PG_DIAG_DATATYPE_NAME"},
    {PG_DIAG_INTERNAL_POSITION, "PG_DIAG_INTERNAL_POSITION"},
    {PG_DIAG_INTERNAL_QUERY, "PG_DIAG_INTERNAL_QUERY"},
    {PG_DIAG_MESSAGE_PRIMARY, "PG_DIAG_MESSAGE_PRIMARY"},
    {PG_DIAG_MESSAGE_DETAIL, "PG_DIAG_MESSAGE_DETAIL"},
    {PG_DIAG_MESSAGE_HINT, "PG_DIAG_MESSAGE_HINT"},
    {PG_DIAG_SEVERITY_NONLOCALIZED, "PG_DIAG_SEVERITY_NONLOCALIZED"},
    {PG_DIAG_SQLSTATE, "PG_DIAG_SQLSTATE"},
    {PG_DIAG_STATEMENT_POSITION, "PG_DIAG_STATEMENT_POSITION"},
    {PG_DIAG_SCHEMA_NAME, "PG_DIAG_SCHEMA_NAME"},
    {PG_DIAG_TABLE_NAME, "PG_DIAG_TABLE_NAME"},
};

AdbcStatusCode ClassifySqlState(const char* sqlstate);

/// \brief Hook that masks sensitive text in SQL before it goes into an error message.
///
/// The hook is only ever called with a bounded prefix of the text (see
/// kMaxQueryBytes and kMaxMessageBytes), so it may use algorithms that are not safe on
/// arbitrarily large input.
using QueryRedactor = std::string (*)(std::string_view query);

/// Longest SQL text kept in an error message. Longer queries are cut before redaction,
/// so nothing past the cut is ever seen, let alone logged.
constexpr size_t kMaxQueryBytes = 2048;
/// Longest server-authored text (messages and diagnostic fields) kept in an error.
constexpr size_t kMaxMessageBytes = 8192;

/// \brief Install the redactor used by RedactQuery. Passing nullptr restores the default,
/// which returns the query unchanged. Call once at driver initialization.
void SetQueryRedactor(QueryRedactor redactor);

/// \brief Truncate SQL text to kMaxQueryBytes, then apply the installed redactor.
///
/// Truncation comes first, so the redactor sees every statement header in the kept
/// prefix and nothing else.
std::string RedactQuery(std::string_view query);

/// \brief As RedactQuery, for text the server wrote (kMaxMessageBytes).
std::string RedactMessage(std::string_view message);

/// \brief The server's error text without the echoed statement.
///
/// libpq's default verbosity appends a "LINE n:" window of the statement around the
/// error position and, for internal queries, a "QUERY:" line. Both can contain
/// credentials, so use the terse form for errors on user-supplied SQL. Falls back to
/// the connection's error message when there is no result.
inline std::string PqErrorText(PGresult* result, PGconn* conn) {
  if (result != nullptr) {
    char* text =
        PQresultVerboseErrorMessage(result, PQERRORS_TERSE, PQSHOW_CONTEXT_NEVER);
    // Never fall back to the default message, which echoes the statement.
    if (text == nullptr) return "(server error text unavailable)";
    std::string out(text);
    PQfreemem(text);
    // The server's own message can quote the statement as well.
    return RedactMessage(out);
  }
  return conn != nullptr ? RedactMessage(PQerrorMessage(conn)) : "";
}

template <typename... Args>
Status MakeStatus(PGresult* result, const char* format_string, Args&&... args) {
  auto message = ::fmt::vformat(format_string, ::fmt::make_format_args(args...));

  AdbcStatusCode code = ADBC_STATUS_IO;
  char sqlstate_out[5];
  std::memset(sqlstate_out, 0, sizeof(sqlstate_out));

  if (result == nullptr) {
    return Status(code, message);
  }

  const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  if (sqlstate) {
    code = ClassifySqlState(sqlstate);
  }

  Status status(code, message);
  if (sqlstate) {
    status.SetSqlState(sqlstate);
  }
  for (const auto& field : kDetailFields) {
    const char* value = PQresultErrorField(result, field.code);
    if (value) {
      status.AddDetail(field.key, RedactMessage(value));
    }
  }

  return status;
}

}  // namespace adbcpq
