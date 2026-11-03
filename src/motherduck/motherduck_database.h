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

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "database.h"

#define ADBC_MOTHERDUCK_OPTION_ATTACH_MODE "adbc.motherduck.attach_mode"
#define ADBC_MOTHERDUCK_OPTION_SESSION_NAME "adbc.motherduck.session_name"
#define ADBC_MOTHERDUCK_OPTION_REGION "adbc.motherduck.region"
/// An alias for the standard ADBC password option, which holds the token.
#define ADBC_MOTHERDUCK_OPTION_TOKEN "adbc.motherduck.token"

namespace adbcpq {

/// \brief The Postgres endpoint host for a MotherDuck region, e.g.
/// pg.us-east-1-aws.motherduck.com for us-east-1.
std::string RegionHost(std::string_view region);

/// \brief Append --<name>=<value> to libpq session options.
///
/// Returns options unchanged if they already set name. Escapes whitespace and
/// backslashes in value, which libpq treats specially.
std::string AddSessionOption(std::string_view options, std::string_view name,
                             std::string_view value);

class MotherduckDatabase : public PostgresDatabase {
 public:
  AdbcStatusCode Init(struct AdbcError* error) override;
  AdbcStatusCode GetOption(const char* option, char* value, size_t* length,
                           struct AdbcError* error) override;
  AdbcStatusCode SetOption(const char* key, const char* value,
                           struct AdbcError* error) override;
  AdbcStatusCode Connect(PGconn** conn, struct AdbcError* error) override;

  /// \brief The libpq keywords and values Connect passes, in order.
  AdbcStatusCode ConnectParams(std::vector<std::pair<std::string, std::string>>* params,
                               struct AdbcError* error) const;

  std::string_view DriverName() override { return "ADBC MotherDuck Driver"; }
  std::string_view DriverVersion() override;

 private:
  std::string attach_mode_ = "single";
  std::string session_name_;
  std::string region_;
  std::string username_;
  std::string password_;
};

}  // namespace adbcpq
