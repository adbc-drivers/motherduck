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

### Browser login

`adbc.motherduck.auth_type` (database option) chooses how the driver gets the
MotherDuck token it sends as the password: `token` (the default) takes it from
the URI, `PGPASSWORD` or `.pgpass`, and `browser` logs in through the browser,
like `motherduck login` and dbt-motherduck's `externalbrowser`, so organizations
that enforce SSO get their identity provider's login page. With `browser`, the
URI must not contain a password, and it may leave out the host, for example
`postgresql:///my_db`.

`AdbcDatabaseInit` logs in if needed: it prints a URL and code to stderr,
opens the browser, and blocks until the login is approved in the browser, for
at most `adbc.motherduck.login_timeout`. It doesn't open a browser on Linux without
`DISPLAY` or `WAYLAND_DISPLAY`; open the printed URL on any device instead.
Like the MotherDuck DuckDB extension's login, it doesn't use an HTTP proxy.

- The login gives a short-lived MotherDuck token, valid for 24 hours; after
  that, the next connection logs in again.
- The token is saved to `~/.motherduck/credentials.json` (or
  `$MOTHERDUCK_HOME/credentials.json`, which must be absolute), readable only
  by the user. The MotherDuck CLI and dbt-motherduck use the same file, so
  logging in with any of them logs in the others, and a saved token is used
  without opening a browser. Processes that use the same file take turns
  logging in, through `credentials.json.lock` next to it, so they share one
  login.
- Without a host in the URI, `PGHOST` or `PGHOSTADDR`, the driver connects to
  the Postgres endpoint of the user's organization, which it looks up at
  `https://api.motherduck.com/mom/lookup-user-with-token`.
- If the endpoint rejects a saved token and the MotherDuck API confirms that
  it expired or was revoked, the driver deletes it and logs in again, once,
  both in `AdbcDatabaseInit` and in `AdbcConnectionInit`.

Related options and environment variables:

| Name | Purpose |
| --- | --- |
| `adbc.motherduck.login_timeout` (database option) | Seconds to wait for the login (default `120`); `0` waits until the code expires, after 15 minutes. |
| `adbc.motherduck.oauth_client_id` (database option) | The Auth0 application to log in through; defaults to MotherDuck's DuckDB extension. |
| `motherduck_host` (environment) | The MotherDuck API host, for example `api.staging.motherduck.com`, as for the CLI. |
| `sslrootcert` (URI) or `PGSSLROOTCERT` (environment) | A CA bundle that the login's HTTPS requests trust too. Otherwise they use the system's trust store. |

Options are read when the database is initialized.

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
