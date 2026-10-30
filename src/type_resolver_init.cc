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

#include "type_resolver_init.h"

#include "codegen/pgtype.h"

using adbc::driver::Status;

namespace adbcpq {

Status InitializeTypeResolver(PostgresTypeResolver& resolver) {
  ArrowError na_error = {};
  for (const auto& item : kBuiltinTypeItems) {
    UNWRAP_NANOARROW(na_error, Internal, resolver.Insert(item, &na_error));
  }
  return Status::Ok();
}

}  // namespace adbcpq

Status InternalAdbcInitializeTypeResolver(adbcpq::PostgresTypeResolver& resolver) {
  return adbcpq::InitializeTypeResolver(resolver);
}
