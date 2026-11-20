<!--
  Copyright (c) 2026 ADBC Drivers Contributors

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-->

# MotherDuck Driver Design

## Code layout

The upstream driver's sources in `src/postgres/` are kept as close to
Apache's as possible, so upstream fixes merge cleanly. MotherDuck behavior
lives in `src/motherduck/`, mostly as subclasses of the upstream classes:

- `motherduck_database`: `MotherduckDatabase`, which adds `attach_mode`,
  `session_name`, `region`, and the token and user handling described in the
  README's Options section, defaults `adbc.postgresql.use_copy` to `false`, ignores
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
- `motherduck_trust_store`: finding the OS's trusted CA certificates for
  libpq, which can't read them itself.
- `motherduck_factory`: the subclasses `postgresql.cc` creates.
- `motherduck.cc`: the `AdbcDriverMotherduckInit` entry point.
- `auth/`: browser login. `login` runs the device flow, the MotherDuck API
  calls and opening the browser; `token_cache` keeps the token in memory and in
  the shared credentials file, and serializes logins within the process and
  across processes; `http_client` and `json` wrap cpp-httplib and yyjson.

The upstream files still differ from Apache's in these ways:

- Every file starts with the modification notice described in the README's License section, and
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

## Upstream history

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
[`ci/upstream-commit-map.txt`](../ci/upstream-commit-map.txt); the full map, including
pruned commits, is retained locally at `.git/filter-repo/commit-map`.
