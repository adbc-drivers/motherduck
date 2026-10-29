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

#include <cctype>
#include <charconv>
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
    std::vector<std::string> statements = SplitStatements(query_);
    if (statements.size() > 1) {
      return ExecutePipeline(statements);
    }

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

Status PqResultHelper::ExecutePipeline(const std::vector<std::string>& statements) {
  ClearResult();
  if (PQenterPipelineMode(conn_) != 1) {
    return MakeStatus(nullptr, "[libpq] Failed to enter pipeline mode: {}",
                      PQerrorMessage(conn_));
  }

  // Queue every statement ahead of one sync, so the server skips the rest
  // once one fails, as it would for a multi-statement simple query.
  Status status;
  for (const auto& statement : statements) {
    if (PQsendQueryParams(conn_, statement.c_str(), /*nParams=*/0,
                          /*paramTypes=*/nullptr, /*paramValues=*/nullptr,
                          /*paramLengths=*/nullptr, /*paramFormats=*/nullptr,
                          static_cast<int>(Format::kText)) != 1) {
      status = MakeStatus(nullptr, "[libpq] Failed to send query '{}': {}", statement,
                          PQerrorMessage(conn_));
      break;
    }
  }
  if (PQpipelineSync(conn_) != 1 && status.ok()) {
    status =
        MakeStatus(nullptr, "[libpq] Failed to sync pipeline: {}", PQerrorMessage(conn_));
  }

  // Drain everything up to the sync, keeping the last statement's result (as
  // PQexec would) or the first error. libpq returns null after each statement's
  // results; two nulls in a row means nothing more is coming, e.g. because the
  // sync was never sent or the connection dropped.
  size_t index = 0;
  bool previous_null = false;
  while (true) {
    PGresult* result = PQgetResult(conn_);
    if (result == nullptr) {
      if (previous_null) break;
      previous_null = true;
      index++;
      continue;
    }
    previous_null = false;

    ExecStatusType result_status = PQresultStatus(result);
    if (result_status == PGRES_PIPELINE_SYNC) {
      PQclear(result);
      break;
    }

    if (!status.ok()) {
      PQclear(result);
    } else if (result_status == PGRES_TUPLES_OK || result_status == PGRES_COMMAND_OK ||
               result_status == PGRES_EMPTY_QUERY) {
      SetResult(result);
    } else {
      SetResult(result);
      status = MakeStatus(result_, "[libpq] Failed to execute query '{}': {}",
                          index < statements.size() ? statements[index] : query_,
                          PQresultErrorMessage(result_));
    }
  }

  if (PQexitPipelineMode(conn_) != 1 && status.ok()) {
    status = MakeStatus(nullptr, "[libpq] Failed to exit pipeline mode: {}",
                        PQerrorMessage(conn_));
  }

  return status;
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

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
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

std::vector<std::string> SplitStatements(std::string_view query) {
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
    const char next = i + 1 < query.size() ? query[i + 1] : '\0';

    if (c == '-' && next == '-') {
      i = query.find('\n', i);
      if (i == std::string_view::npos) i = query.size();
      continue;
    }
    if (c == '/' && next == '*') {
      // Block comments nest.
      int depth = 1;
      i += 2;
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
      i = SkipQuoted(query, i, escape_string);
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

}  // namespace adbcpq
