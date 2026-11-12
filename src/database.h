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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "auth/token_cache.h"
#include "driver/framework/status.h"
#include "postgres_type.h"
#include "type_resolver_init.h"

#define ADBC_MOTHERDUCK_OPTION_ATTACH_MODE "adbc.motherduck.attach_mode"
#define ADBC_MOTHERDUCK_OPTION_AUTH_TYPE "adbc.motherduck.auth_type"
#define ADBC_MOTHERDUCK_OPTION_OAUTH_CLIENT_ID "adbc.motherduck.oauth_client_id"
#define ADBC_MOTHERDUCK_OPTION_LOGIN_TIMEOUT "adbc.motherduck.login_timeout"

namespace adbcpq {
using adbc::driver::Status;

/// \brief libpq connection parameters, as keyword/value pairs.
using ConnectParams = std::vector<std::pair<std::string, std::string>>;

/// \brief Append --attach_mode=<attach_mode> to libpq session options.
///
/// Returns options unchanged if they already set attach_mode.
std::string AddAttachMode(std::string_view options, std::string_view attach_mode);

/// \brief Whether a libpq connection error may mean the endpoint rejected the
///   token, so it's worth asking the MotherDuck API.
///
/// libpq drops the SQLSTATE of errors raised while connecting, so this matches
/// the messages of MotherDuck's Postgres endpoint.
bool IsAuthFailure(std::string_view message);

/// \brief The parameters with browser login's password and, if need_host, host.
ConnectParams AddCredentials(ConnectParams params, const auth::Credentials& credentials,
                             bool need_host);

class PostgresDatabase {
 public:
  PostgresDatabase();
  ~PostgresDatabase();

  // Public ADBC API

  AdbcStatusCode Init(struct AdbcError* error);
  AdbcStatusCode Release(struct AdbcError* error);
  AdbcStatusCode GetOption(const char* option, char* value, size_t* length,
                           struct AdbcError* error);
  AdbcStatusCode GetOptionBytes(const char* option, uint8_t* value, size_t* length,
                                struct AdbcError* error);
  AdbcStatusCode GetOptionDouble(const char* option, double* value,
                                 struct AdbcError* error);
  AdbcStatusCode GetOptionInt(const char* option, int64_t* value,
                              struct AdbcError* error);
  AdbcStatusCode SetOption(const char* key, const char* value, struct AdbcError* error);
  AdbcStatusCode SetOptionBytes(const char* key, const uint8_t* value, size_t length,
                                struct AdbcError* error);
  AdbcStatusCode SetOptionDouble(const char* key, double value, struct AdbcError* error);
  AdbcStatusCode SetOptionInt(const char* key, int64_t value, struct AdbcError* error);

  // Internal implementation

  AdbcStatusCode Connect(PGconn** conn, struct AdbcError* error);
  AdbcStatusCode Disconnect(PGconn** conn, struct AdbcError* error);
  const std::shared_ptr<PostgresTypeResolver>& type_resolver() const {
    return type_resolver_;
  }

  std::string_view VendorName() { return "PostgreSQL"; }
  bool use_copy() const { return use_copy_; }

 private:
  int32_t open_connections_;
  std::string uri_;
  std::shared_ptr<PostgresTypeResolver> type_resolver_;
  bool use_copy_ = true;
  std::string attach_mode_ = "single";
  std::string auth_type_ = "token";
  std::string oauth_client_id_{auth::kDefaultClientId};
  int64_t login_timeout_seconds_ = 120;
  // Set by Init when auth_type is browser.
  std::unique_ptr<auth::TokenCache> token_cache_;
};
}  // namespace adbcpq
