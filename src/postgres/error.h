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
#include <type_traits>
#include <utility>
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

/// \brief Hook that masks sensitive text before it goes into an error message.
///
/// One is installed for SQL (SetQueryRedactor) and optionally another for text the
/// server wrote (SetMessageRedactor). It only sees text already cut to kMaxQueryBytes
/// or kMaxMessageBytes.
using TextRedactor = std::string (*)(std::string_view text);

/// Longest SQL text kept in an error message; the rest is cut before redaction.
constexpr size_t kMaxQueryBytes = 512 * 1024;
/// Longest server text (messages and diagnostic fields) kept in an error.
constexpr size_t kMaxMessageBytes = 8192;

/// \brief Text that is safe to put in an error message.
///
/// RedactQuery, RedactMessage and PqErrorText return it, and MakeStatus accepts nothing
/// else as an argument, so raw SQL or PQerrorMessage can't slip into an error. Wrapping
/// text that is already safe, like a status name, is explicit.
class RedactedText : public std::string {
 public:
  RedactedText() = default;
  explicit RedactedText(std::string text) : std::string(std::move(text)) {}
  explicit RedactedText(const char* text) : std::string(text) {}
};

}  // namespace adbcpq

// fmt matches exact types, so a std::string subclass needs its own formatter.
template <>
struct fmt::formatter<adbcpq::RedactedText> : fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const adbcpq::RedactedText& text, FormatContext& ctx) const {
    return fmt::formatter<std::string_view>::format(std::string_view(text), ctx);
  }
};

namespace adbcpq {

/// \brief Install the redactor used by RedactQuery (nullptr: none). Call at init.
void SetQueryRedactor(TextRedactor redactor);

/// \brief Install the redactor used by RedactMessage, for text the server wrote.
///
/// nullptr (the default) means RedactMessage uses the query redactor. Call at init,
/// before SetQueryRedactor, so a message is never redacted with only the query one.
void SetMessageRedactor(TextRedactor redactor);

/// \brief Truncate SQL text to kMaxQueryBytes, then apply the query redactor.
///
/// Truncation comes first, so the redactor never sees text past the cut.
RedactedText RedactQuery(std::string_view query);

/// \brief Truncate server text to kMaxMessageBytes, then apply the message redactor,
/// or the query redactor if no message redactor is installed.
RedactedText RedactMessage(std::string_view message);

/// \brief The server's error text without the echoed statement.
///
/// libpq's default verbosity echoes the statement ("LINE n:", "QUERY:"), which can hold
/// credentials, so this uses the terse form. Without a result, or when the result has no
/// fields, it uses the connection's message.
inline RedactedText PqErrorText(PGresult* result, PGconn* conn) {
  if (result != nullptr) {
    // Results libpq makes itself (lost connection) have no fields, so the verbose form
    // says "missing error text". Their own message has no echo to leak.
    if (PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY) == nullptr) {
      std::string_view own = PQresultErrorMessage(result);
      if (!own.empty()) return RedactMessage(own);
      return conn != nullptr ? RedactMessage(PQerrorMessage(conn)) : RedactedText();
    }
    char* text =
        PQresultVerboseErrorMessage(result, PQERRORS_TERSE, PQSHOW_CONTEXT_NEVER);
    // Don't fall back to the default message: it echoes the statement.
    if (text == nullptr) return RedactedText("(server error text unavailable)");
    std::string out(text);
    PQfreemem(text);
    // The message itself can quote the statement too.
    return RedactMessage(out);
  }
  return conn != nullptr ? RedactMessage(PQerrorMessage(conn)) : RedactedText();
}

/// \brief libpq's name for a result status, e.g. "PGRES_FATAL_ERROR".
inline RedactedText PqStatusName(ExecStatusType status) {
  return RedactedText(PQresStatus(status));
}

/// \brief Quote an identifier with PQescapeIdentifier, storing the result in *out.
///
/// libpq rejects identifiers that are not valid in the client encoding, so a
/// failure on a healthy connection is reported as ADBC_STATUS_INVALID_ARGUMENT
/// (libpq also fails if it runs out of memory, which is rare enough not to
/// distinguish). Without a healthy connection it is ADBC_STATUS_INTERNAL.
AdbcStatusCode EscapeIdentifier(PGconn* conn, std::string_view identifier,
                                std::string* out, struct AdbcError* error);

/// \brief The status for a server error, from its SQLSTATE and primary message.
///
/// Defined by the driver built on this one (motherduck/motherduck_error.cc), so
/// it can classify errors its server reports with a generic SQLSTATE.
AdbcStatusCode ClassifyServerError(const char* sqlstate, const char* primary_message);

template <typename... Args>
Status MakeStatus(PGresult* result, const char* format_string, Args&&... args) {
  static_assert((... && (std::is_arithmetic_v<std::decay_t<Args>> ||
                         std::is_same_v<std::decay_t<Args>, RedactedText>)),
                "MakeStatus arguments must be numbers or RedactedText: pass SQL through "
                "RedactQuery and libpq errors through PqErrorText");
  auto message = ::fmt::vformat(format_string, ::fmt::make_format_args(args...));

  AdbcStatusCode code = ADBC_STATUS_IO;
  char sqlstate_out[5];
  std::memset(sqlstate_out, 0, sizeof(sqlstate_out));

  if (result == nullptr) {
    return Status(code, message);
  }

  const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  if (sqlstate) {
    code = ClassifyServerError(sqlstate,
                               PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY));
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
