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

#include <arrow-adbc/adbc.h>
#include <libpq-fe.h>

#include "database.h"

#define ADBC_MOTHERDUCK_OPTION_ATTACH_MODE "adbc.motherduck.attach_mode"

namespace adbcpq {

/// \brief Append --attach_mode=<attach_mode> to libpq session options.
///
/// Returns options unchanged if they already set attach_mode.
std::string AddAttachMode(std::string_view options, std::string_view attach_mode);

class MotherduckDatabase : public PostgresDatabase {
 public:
  AdbcStatusCode Init(struct AdbcError* error) override;
  AdbcStatusCode GetOption(const char* option, char* value, size_t* length,
                           struct AdbcError* error) override;
  AdbcStatusCode SetOption(const char* key, const char* value,
                           struct AdbcError* error) override;
  AdbcStatusCode Connect(PGconn** conn, struct AdbcError* error) override;

  std::string_view DriverName() override { return "ADBC MotherDuck Driver"; }
  std::string_view DriverVersion() override;

 private:
  std::string attach_mode_ = "single";
};

}  // namespace adbcpq
