<!---
  Copyright (c) 2026 ADBC Drivers Contributors

  This file has been modified from its original version, which is
  under the Apache License:

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# ADBC Driver for MotherDuck

A C++ ADBC driver for MotherDuck's PostgreSQL protocol endpoint, forked from
Apache Arrow ADBC's libpq-based PostgreSQL driver. This is the initial fork;
MotherDuck-specific protocol and authentication changes remain to be implemented.
The upstream PostgreSQL driver's approach credits [pgeon](https://github.com/0x0L/pgeon).

## Build

Requires CMake 3.24+, a C++20 compiler, and PostgreSQL's client library and
headers (libpq). The driver bundles the Apache common/framework code, nanoarrow,
fmt, portable-snippets, and boost.int128 it needs. Test builds also require
GoogleTest/GoogleMock; CMake downloads GoogleTest 1.15.2 if it is not installed.

For a native development build, set `PostgreSQL_ROOT` or `CMAKE_PREFIX_PATH`
if CMake cannot find libpq, then run:

```sh
./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64
```

Use `linux` or `windows` and `amd64` or `arm64` as appropriate. With Homebrew,
for example, set `PostgreSQL_ROOT="$(brew --prefix libpq)"` after installing
`libpq`. An existing Homebrew PostgreSQL installation also supplies libpq.

For a release build without test dependencies:

```sh
./ci/scripts/build.sh release macos arm64
```

The scripts copy the library to `build/libadbc_driver_motherduck.dylib` on
macOS, `.so` on Linux, or `.dll` on Windows. The entry point inferred by ADBC
driver managers is `AdbcDriverMotherduckInit`. The original PostgreSQL entry
point and the generic `AdbcDriverInit` remain available.

For Foundry packaging, use the vcpkg toolchain through `CMAKE_TOOLCHAIN_FILE`,
`VCPKG_ROOT`, or `VCPKG_INSTALLATION_ROOT`. The scripts choose static vcpkg
triplets, including a static runtime on Windows. A native build with a system
libpq is suitable for local development and retains that runtime dependency.

Direct CMake builds are also supported:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
cmake --install build/debug --prefix build/install
```

## Inherited tests

The complete upstream C++ test suites are compiled in test builds. Linux test
builds enable PostgreSQL integration tests by default; macOS and Windows run
server-independent tests by default. Use the disposable Compose service locally:

```sh
docker compose up --detach --wait test-service
set -a
source .env.linux
set +a
./ci/scripts/build.sh test linux amd64
./ci/scripts/test.sh linux amd64
docker compose down --volumes
```

The service uses `postgres:latest` with database/user `postgres` and password
`password` on localhost port 5432. Set `MOTHERDUCK_TEST_POSTGRESQL=OFF` when
building to disable database tests, or `ON` to enable them on another platform
with an available PostgreSQL server. CI starts the service and loads `.env.linux`
only on Linux.

In CI, the C++ tests run against MotherDuck's Postgres endpoint instead of the
Compose service. The test job uses the `MOTHERDUCK_TOKEN` secret from the
`test` GitHub environment; `ci/scripts/pre-test.sh` builds the connection URI
from it, and `MOTHERDUCK_PG_HOST` and `MOTHERDUCK_TEST_DATABASE` override the
default host (`pg.us-east-1-aws.motherduck.com`) and database (`md:`). The
validation suite still uses the Compose service.

These tests exercise PostgreSQL compatibility. The inherited Python files in
`validation/` still target PostgreSQL; MotherDuck validation and published
validation documentation will be configured separately.

## Upstream history and licensing

The fork was extracted from `apache/arrow-adbc` at
`b519e662f9e368c9dfbef313d0914b6afe50fe37` (2026-10-26), retaining 373 commits
for the driver and its bundled Apache dependencies. The original driver
history begins with the libpq implementation on 2022-08-26. Historical paths
`c/drivers/postgres/`, `c/driver/postgres/`, and `c/driver/postgresql/` are all
included. Driver sources are now in `src/`, C++ tests in `tests/`, and shared
Apache code in `third_party/arrow-adbc/`.

To reproduce the extraction in a fresh, full-history clone of Apache Arrow ADBC
checked out at that commit, use this repository's `ci/upstream-paths.txt`:

```sh
git filter-repo --paths-from-file /path/to/motherduck/ci/upstream-paths.txt \
  --preserve-commit-hashes --preserve-commit-encoding
```

Filtering changes commit IDs. Authors, committers, dates, and commit messages
are preserved. The retained commits are mapped in
[`ci/upstream-commit-map.txt`](ci/upstream-commit-map.txt); the full map, including
pruned commits, is retained locally at `.git/filter-repo/commit-map`.

Imported ASF headers retain their original text, preceded by a 2026 ADBC
Drivers Contributors copyright and modification notice. Other bundled
third-party notices remain intact. See `LICENSE.txt` and `NOTICE.txt`.
