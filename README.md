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

## Options

`adbc.motherduck.attach_mode` (database option) sets the MotherDuck
[attach mode](https://motherduck.com/docs/sql-reference/postgres-endpoint/#session-options):
`single` (the default) attaches only the database named in the connection,
and `workspace` attaches every database in the account. The driver adds
`--attach_mode=<mode>` to libpq's `options`, after any session options from
the URI or `PGOPTIONS`. If those already set `attach_mode`, they win.
Single mode needs a named database; to connect to `md:`, set `workspace`.

libpq connection service files (`~/.pg_service.conf`, `service=`, or
`PGSERVICE`) aren't supported: the driver always passes `options`, which
replaces any `options` a service file sets. Put connection settings in the
URI instead.

## Unsupported PostgreSQL driver features

These upstream PostgreSQL driver features don't work against MotherDuck's
Postgres endpoint yet. Most are endpoint gaps; the tests skip or expect them
to fail.

- **Bulk ingest** fails: the endpoint has no `COPY ... FROM STDIN` (#31).
  `adbc.ingest.target_catalog` isn't supported (#16), appending to an
  existing table fails its `$1::regclass` lookup (#13), and temporary tables
  aren't available because the endpoint rejects `CREATE TEMP TABLE`.
- **COPY result reads** fail: the endpoint rejects binary
  `COPY ... TO STDOUT`, so set `adbc.postgresql.use_copy` to `false` (#11).
  The non-COPY path fetches the whole result at once, so it ignores
  `adbc.postgresql.batch_size_hint_bytes`, the connection reports idle in
  a transaction rather than active while you read (#11), and a running
  query can't be cancelled (#48).
- **Parameterized queries** fail or return wrong results (#17), and
  `GetParameterSchema` fails on queries whose result types the endpoint
  can't infer.
- **Rows affected** is wrong for `DELETE` (#19), and is the row count rather
  than -1 for a `SELECT` on the non-COPY path (#50). `ExecuteQuery` on an
  `INSERT` also returns the endpoint's `Count` row as a batch (#49).
- **Metadata:** `GetObjects` and the current catalog don't reflect MotherDuck
  databases and schemas (#14), `GetTableSchema` fails its `$1::regclass`
  lookup (#13), `GetInfo` reports PostgreSQL and queries
  `server_version_num` (#15), and `GetStatistics` isn't supported (it reads
  `pg_stats`).
- **Transactions:** DuckDB has a single isolation level and the endpoint
  rejects `SET TRANSACTION`, so isolation levels can't be set.
- **Types:** PostgreSQL-only types such as `jsonb` and `int2vector` don't
  exist, DuckDB `DECIMAL` holds at most 38 digits, and lists of decimals
  can't be bound (#37).
- **Errors:** data conversion errors map to `INTERNAL` instead of
  `INVALID_DATA` (#32).

## Inherited tests

The complete upstream C++ test suites are compiled in test builds. The
integration tests and the Python validation suite run against MotherDuck's
Postgres endpoint; plain PostgreSQL isn't supported, because every connection
sets `attach_mode`. Linux test builds enable the integration tests by default,
and macOS and Windows run only server-independent tests unless you set
`MOTHERDUCK_TEST_POSTGRESQL=ON` when building.

To run them locally, export a MotherDuck token and let `ci/scripts/pre-test.sh`
write the connection settings to `.env.override` (it needs `pixi install` for
its CA bundle):

```sh
export MOTHERDUCK_TOKEN=...
./ci/scripts/pre-test.sh macos arm64
set -a
source .env.override
set +a
MOTHERDUCK_TEST_POSTGRESQL=ON ./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64
pixi run validate
```

By default each run gets a fresh database, `adbc_ci_<run>_<job>_<platform>_<arch>`,
which the tests create before they start and drop when they finish, so
concurrent jobs and runs don't share tables. Set `MOTHERDUCK_TEST_DATABASE` to
an existing database to use it instead; the tests leave it in place.
`MOTHERDUCK_PG_HOST` overrides the default host
(`pg.us-east-1-aws.motherduck.com`). Test connections use the driver's default
`attach_mode=single`, so other databases in the account stay out of catalog
queries.

CI does the same, using the `MOTHERDUCK_TOKEN` secret from the `test` GitHub
environment.

The validation suite's quirks in `validation/tests/postgresql.py` describe
MotherDuck: its catalog, `main` schema, no temporary tables, and the vendor
name `MotherDuck`. The driver still reports itself as PostgreSQL (#15), so the
vendor check fails until that's fixed. Published validation documentation will
be configured separately.

## Code layout

The upstream driver's sources in `src/postgres/` are kept as close to
Apache's as possible, so upstream fixes merge cleanly. MotherDuck behavior
lives in `src/motherduck/`, mostly as subclasses of the upstream classes:

- `motherduck_database`: `MotherduckDatabase`, which adds `attach_mode`,
  ignores `adbc.postgresql.type_resolver_mode`, uses the built-in type table
  instead of reading the server's, and reports the MotherDuck driver name and
  version.
- `motherduck_connection`: `MotherduckConnection`, which leaves the vendor
  version out of `GetInfo` rather than report the endpoint's PostgreSQL
  `server_version_num`.
- `motherduck_statement`: `MotherduckStatement`, which accepts the result sets
  MotherDuck returns for DDL, and `MotherduckResultReader`, which runs queries
  without a result through a pipeline.
- `motherduck_pipeline`: pipelined execution and splitting multi-statement
  queries.
- `motherduck_type_resolver`: DuckDB-only type OIDs.
- `motherduck_factory`: the subclasses `postgresql.cc` creates.
- `motherduck.cc`: the `AdbcDriverMotherduckInit` entry point.

The upstream files still differ from Apache's in these ways:

- Every file starts with the modification notice described below, and
  includes use paths relative to `src/postgres/`.
- Hooks for the subclasses: `virtual` methods and `protected` members in
  `PostgresDatabase`, `PostgresConnection`, `PostgresStatement`, and
  `PqResultArrayReader`;
  `DriverName()` and `DriverVersion()`, which `GetInfo` reports; and the
  factory calls in `postgresql.cc`.
- `BindStream::SerializeCurrentRow()` is split out of
  `BindAndExecuteCurrentRow()` so rows can also be sent in a pipeline.
- A failed bound row rolls back the transaction begun to set the time zone
  (`BindStream::Abort()`); upstream leaves it open. With autocommit off,
  `Abort()` instead restores the time zone, unless the user's transaction has
  failed. Every error from `SetParamTypes()` onward while executing bound
  rows calls `Abort()` (`PqResultArrayReader::AbortBindOnError()`), so the
  transaction `SetParamTypes()` may begin isn't left open. This is a
  candidate to contribute upstream, as is the `SerializeCurrentRow()` split.
- `PqResultArrayReader` checks that each bound row's result has the first
  result's column count and types (`CheckResultMatches()`) before decoding it
  into arrays built for the first result; upstream doesn't check.

When merging from upstream, keep these differences and put new MotherDuck
behavior in `src/motherduck/`. MotherDuck's own tests are in
`tests/motherduck_*_test.cc`; upstream tests that don't apply are skipped
through `MOTHERDUCK_SKIPPED_UPSTREAM_TESTS` in `tests/CMakeLists.txt`.

## Upstream history and licensing

The fork was extracted from `apache/arrow-adbc` at
`b519e662f9e368c9dfbef313d0914b6afe50fe37` (2026-10-26), retaining 373 commits
for the driver and its bundled Apache dependencies. The original driver
history begins with the libpq implementation on 2022-08-26. Historical paths
`c/drivers/postgres/`, `c/driver/postgres/`, and `c/driver/postgresql/` are all
included. The extraction puts driver sources in `src/` (since moved to
`src/postgres/`), C++ tests in `tests/`, and shared Apache code in
`third_party/arrow-adbc/`.

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
