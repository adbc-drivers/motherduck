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

Requires CMake 3.24+, a C++20 compiler, and the client libraries and headers
of PostgreSQL (libpq) and OpenSSL. The driver bundles the Apache
common/framework code, nanoarrow, fmt, portable-snippets, boost.int128,
cpp-httplib, and yyjson it needs. Test builds also require
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
driver managers is `AdbcDriverMotherduckInit`, and the generic `AdbcDriverInit`
forwards to it. The original PostgreSQL entry point `AdbcDriverPostgresqlInit`
remains available and does not install the MotherDuck error-message redaction.

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
`--attach_mode=<mode>` to libpq's `options`. MotherDuck keeps the last value
of a repeated setting, so the default goes before any session options from the
URI or `PGOPTIONS`, which can override it, and an explicitly set option goes
after them, overriding them. Session options that end in a backslash escaping
nothing are rejected, since it would escape the space before the driver's
options. Single mode needs a named database; to connect to `md:`, set
`workspace`.

`adbc.motherduck.session_name` (database option) sets MotherDuck's
[session name](https://motherduck.com/docs/sql-reference/connection-string-parameters/#session_name),
which routes connections with the same name to the same replica under read
scaling and appears in query history. The driver adds `--session_name=<name>`
after the URI's session options, escaping whitespace and backslashes with a
backslash; MotherDuck removes the escapes, so `my session` arrives as is. It's
unset by default; an empty value unsets it.

MotherDuck builds the user agent in query history and its logs from libpq's
`application_name`. The driver adds `adbc_driver_motherduck/<version>` to it,
after the application's own name if the URI's `application_name` or
`fallback_application_name` gives one (in that order), so
`application_name=dbt` becomes `dbt adbc_driver_motherduck/<version>`.
`PGAPPNAME` is never used.

The MotherDuck token is the connection's password. The driver takes it from,
in order: the standard ADBC `password` database option (or its alias,
`adbc.motherduck.token`), a password in the URI, and the `MOTHERDUCK_TOKEN`
environment variable. It always passes the token to libpq, so `PGPASSWORD` and
password files (`~/.pgpass`, `PGPASSFILE`) are never used. Without a token,
connecting fails with `UNAUTHENTICATED`.

The user is `postgres`, which MotherDuck's endpoint expects, so the URI can
leave it out. The standard ADBC `username` database option, then a user in the
URI, replace it. `PGUSER` is never used. Reading the `username` option returns
the user the driver connects as.

`adbc.motherduck.region` (database option) sets the host from a MotherDuck
region, so the URI can leave it out: `us-east-1` connects to
`pg.us-east-1-aws.motherduck.com`, and `eastus2` connects to
`pg.eastus2-azure.motherduck.com`. Find your region with `md_user_info()`. A
region is dash-separated parts of letters and digits, which the driver
lowercases. One with
dashes is taken to be on AWS and one without on Azure, unless it ends in
`-aws` or `-azure`. The driver doesn't check the region against a list, so a
mistyped one fails at connect with a DNS error naming the host. Setting it
together with a different host or any `hostaddr` in the URI, with
`PGHOSTADDR`, or with a connection service (`service=` or `PGSERVICE`, which
can set `hostaddr`) is an error; a host that matches the region, ignoring case
and a trailing dot, is allowed. When it's set, `PGHOST` and `PGPORT` are
ignored, and the port is 5432 unless the URI sets one. For example, with
`region` set to `us-east-1`:

```text
postgresql:///my_db?sslmode=verify-full&sslrootcert=system
```

Unless the URI or `PGSSLMODE` sets `sslmode`, the driver uses
`sslmode=require` instead of libpq's default, `prefer`, so the token is never
sent unencrypted. `require` doesn't verify the server's certificate; set
`sslmode=verify-full` to verify it. With `sslrootcert=system` (in the URI or
`PGSSLROOTCERT`), libpq 16 and later already default to `verify-full`, so the
driver leaves `sslmode` alone. Naming a connection service also turns this
default off, since the service file may set `sslmode` itself.

libpq connection service files (`~/.pg_service.conf`, `service=`, or
`PGSERVICE`) aren't supported: the driver always passes `options`, which
replaces any `options` a service file sets, doesn't default `sslmode` when
one is named, and can't combine one with a `region`. Put connection settings
in the URI instead.

### Browser login

`adbc.motherduck.auth_type` (database option) chooses how the driver gets the
MotherDuck token it sends as the password: `token` (the default) takes it as
described above, and `browser` logs in through the browser, so organizations
that enforce SSO get their identity provider's login page. With `browser`, set no
token (no `password` or `adbc.motherduck.token` option, and no password in the
URI; `MOTHERDUCK_TOKEN` is ignored), and the URI may leave out the host, for
example `postgresql:///my_db`.

`AdbcDatabaseInit` logs in if needed: it prints a URL and code to stderr,
opens the browser, and blocks until the login is approved in the browser, for
at most `adbc.motherduck.login_timeout`. It doesn't open a browser on Linux without
`DISPLAY` or `WAYLAND_DISPLAY`; open the printed URL on any device instead.
Like the MotherDuck DuckDB extension's login, it doesn't use an HTTP proxy.

- The login gives a short-lived MotherDuck token, valid for 24 hours; after
  that, the next connection logs in again.
- The token is saved to `~/.motherduck/credentials.json` (or
  `$MOTHERDUCK_HOME/credentials.json`, which must be absolute), readable only
  by the user, and a saved token is used without opening a browser. The driver
  reads and writes only the file's top-level `motherduck_token` (in a
  version 1 file) and keeps everything else in it, such as other tools' saved
  profiles; a rejected token is removed from the file, not the whole file.
  Processes that use the same file take turns logging in, through `credentials.json.lock`
  next to it, so they share one login. The process logging in writes its
  process ID and when its login ends at the latest to `credentials.json.login`.
  The others wait for that login as long as it takes, and give up once it has
  been overdue for 10 seconds, naming the stuck process. They wait at most
  `login_timeout` (or 30 seconds with `0`) for a process that holds the lock
  without logging in.
- Connections opened at the same time in one process, such as one per thread,
  share one login: they wait for it as long as it takes, then use its token. If
  it fails (it times out or is denied), they all fail with its error instead of
  opening the browser again.
- After a login is denied, or finds no MotherDuck account, new connections in
  the process that log in to the same account (the same credentials file,
  `oauth_client_id` and API host), from any database, fail with that error
  for 60 seconds instead of opening the browser again. A login that times out
  doesn't pause anything, and databases set up differently don't share each
  other's failures.
- Without a host from the URI, `PGHOST`, `PGHOSTADDR`, `adbc.motherduck.region`
  or a connection service, the driver connects to the Postgres endpoint of the
  user's organization, which it looks up at
  `https://api.motherduck.com/mom/lookup-user-with-token`.
- If the endpoint rejects a saved token and the MotherDuck API confirms that
  it expired or was revoked, the driver deletes it and logs in again, once,
  both in `AdbcDatabaseInit` and in `AdbcConnectionInit`.

Related options and environment variables:

| Name | Purpose |
| --- | --- |
| `adbc.motherduck.login_timeout` (database option) | Seconds to wait for the login (default `120`); `0` waits until the code expires, after 15 minutes. |
| `adbc.motherduck.oauth_client_id` (database option) | The Auth0 application to log in through |
| `motherduck_host` (environment) | The MotherDuck API host, for example `api.staging.motherduck.com`. |
| `sslrootcert` (URI) or `PGSSLROOTCERT` (environment) | A CA bundle that the login's HTTPS requests trust too. Otherwise they use the system's trust store. |

Options are read when the database is initialized.

## Unsupported PostgreSQL driver features

These upstream PostgreSQL driver features don't work against MotherDuck's
Postgres endpoint yet. Most are endpoint gaps; the tests skip or expect them
to fail.

- **Bulk ingest** fails: the endpoint has no `COPY ... FROM STDIN` (#31).
  `adbc.ingest.target_catalog` isn't supported (#16), appending to an
  existing table fails its `$1::regclass` lookup (#13), and temporary tables
  aren't available because the endpoint rejects `CREATE TEMP TABLE`.
- **COPY result reads** fail: the endpoint rejects binary
  `COPY ... TO STDOUT`, so the driver defaults `adbc.postgresql.use_copy` to
  `false` (#11); setting it to `true` fails. The non-COPY path fetches the
  whole result at once, so it ignores `adbc.postgresql.batch_size_hint_bytes`,
  the connection reports idle in a transaction rather than active while you
  read (#11), and a running query can't be cancelled (#48).
- **Parameter types:** the endpoint can't prepare a query whose result
  column types depend on its parameters, as in `SELECT $1`, and ignores the
  parameter types sent with the prepare (#17). When that happens with
  parameters bound, the driver prepares the query again with each `$n` cast to
  its bound type, so such queries work once parameters are bound; without
  bound parameters, `GetParameterSchema` and `ExecuteSchema` return
  `NOT_IMPLEMENTED` for them. An all-null parameter whose type the endpoint
  can't infer is cast to `TEXT`, so `SELECT 1 + $1` with a null fails.
- **Rows affected** is wrong for `DELETE` (#19), and is the row count rather
  than -1 for a `SELECT` on the non-COPY path (#50). `ExecuteQuery` on an
  `INSERT` also returns the endpoint's `Count` row as a batch (#49).
- **Metadata:** the current catalog reports `md:` rather than the current
  database when you connect to `md:` (#14), and `GetStatistics` and
  `GetStatisticNames` aren't supported, since MotherDuck has no `pg_stats`.
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
  `session_name`, `region`, and the token and user handling described under
  Options, defaults `adbc.postgresql.use_copy` to `false`, ignores
  `adbc.postgresql.type_resolver_mode`, uses the built-in type table
  instead of reading the server's, and reports the MotherDuck driver name and
  version.
- `motherduck_connection`: `MotherduckConnection`, which leaves the vendor
  version out of `GetInfo` rather than report the endpoint's PostgreSQL
  `server_version_num`, describes `SELECT * FROM` the table for
  `GetTableSchema`, reports only `table` and `view` table types, and rejects
  the statistics calls.
- `motherduck_get_objects`: the `GetObjects` helper, which reads DuckDB's
  `duckdb_*()` metadata functions for every attached database, one query per
  level, with case-insensitive filters.
- `motherduck_error`: `ClassifyServerError`, which classifies the endpoint's
  missing-object errors as `NOT_FOUND` although they arrive with the generic
  SQLSTATE `XXUUU`.
- `motherduck_statement`: `MotherduckStatement`, which accepts the result sets
  MotherDuck returns for DDL, and `MotherduckResultReader`, which runs queries
  without a result through a pipeline. Both cast parameters to their bound
  types when the endpoint can't prepare a query without them.
- `motherduck_pipeline`: pipelined execution, splitting multi-statement
  queries, and casting a query's parameters.
- `motherduck_type_resolver`: DuckDB-only type OIDs.
- `motherduck_factory`: the subclasses `postgresql.cc` creates.
- `motherduck.cc`: the `AdbcDriverMotherduckInit` entry point.
- `auth/`: browser login. `login` runs the device flow, the MotherDuck API
  calls and opening the browser; `token_cache` keeps the token in memory and in
  the shared credentials file, and serializes logins; `http_client` and `json`
  wrap cpp-httplib and yyjson.

The upstream files still differ from Apache's in these ways:

- Every file starts with the modification notice described below, and
  includes use paths relative to `src/postgres/`.
- Hooks for the subclasses: `virtual` methods and `protected` members in
  `PostgresDatabase`, `PostgresConnection`, `PostgresStatement`, and
  `PqResultArrayReader`; `MakeGetObjectsHelper()`, which lets a subclass
  supply the `GetObjects` helper; `ClassifyServerError()`, which `MakeStatus`
  calls and `motherduck_error.cc` defines;
  `DriverName()` and `DriverVersion()`, which `GetInfo` reports; and the
  factory calls in `postgresql.cc`.
- `BindStream::SerializeCurrentRow()` is split out of
  `BindAndExecuteCurrentRow()` so rows can also be sent in a pipeline.
- A failed bound row rolls back the transaction begun to set the time zone
  (`BindStream::Abort()`); upstream leaves it open. This is a candidate to
  contribute upstream, as is the `SerializeCurrentRow()` split.

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
