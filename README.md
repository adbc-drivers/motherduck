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

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/motherduck-logo-dark.svg">
    <img src="docs/images/motherduck-logo.svg" alt="MotherDuck" width="320">
  </picture>
</p>

# ADBC Driver for MotherDuck

An [ADBC](https://arrow.apache.org/adbc/) driver for
[MotherDuck](https://motherduck.com), implemented in C++. It connects through
MotherDuck's PostgreSQL protocol endpoint and is forked from Apache Arrow ADBC's
libpq-based PostgreSQL driver. The upstream PostgreSQL driver's approach credits
[pgeon](https://github.com/0x0L/pgeon).

> [!IMPORTANT]
> This driver is **not** a drop-in replacement for the ADBC PostgreSQL driver,
> although it is built from it and talks to MotherDuck's PostgreSQL protocol
> endpoint. It is a separate driver with its own name and entry point, it only
> works with MotherDuck, and some of the PostgreSQL driver's options and
> features behave differently or don't work. See
> [Differences from the PostgreSQL driver](#differences-from-the-postgresql-driver).

## Installation

Released builds are published for various platforms on the
[Columnar](https://columnar.tech) CDN. They can be installed by any tool that
supports [ADBC](https://arrow.apache.org/adbc/) Driver Manifests, such as
[dbc](https://columnar.tech/dbc):

```sh
dbc install motherduck --pre
```

> **Note:** The driver hasn't been published to the Columnar registry yet, so
> this command won't find it until the first release.

Until then, or if you would rather build the driver yourself, see
[Building](#building).

## Connecting

The driver uses your MotherDuck token as the password. Export it, then give the
database to connect to and your account's region (find yours with
`md_user_info()`):

```sh
export MOTHERDUCK_TOKEN=...
pip install adbc-driver-manager pyarrow
```

```python
from adbc_driver_manager import dbapi

with dbapi.connect(
    driver="motherduck",
    db_kwargs={
        "uri": "postgresql:///my_db",
        "adbc.motherduck.region": "us-east-1",
    },
) as conn:
    with conn.cursor() as cursor:
        cursor.execute("SELECT 42")
        print(cursor.fetch_arrow_table())
```

The region sets the host, so the URI can leave it out: `us-east-1` connects to
`pg.us-east-1-aws.motherduck.com`. Without the option, put the host in the URI,
as in `postgresql://pg.us-east-1-aws.motherduck.com/my_db`. To use a driver you
built yourself, pass the path to the library as `driver`, such as
`build/libadbc_driver_motherduck.dylib`.

The URI is a libpq connection URI (`postgresql://[user[:password]@][host][:port]/database[?parameters]`).
It can also start with `motherduck://` instead, as in `motherduck:///my_db`:
passed in place of a driver name, as in `dbapi.connect("motherduck:///my_db")`,
an ADBC driver manager loads this driver from the URI's scheme and passes it
the whole URI. The driver reads `motherduck://` as `postgresql://`; the rest of
the URI is the same. A `motherduck://` URI must
name a host or be used with `adbc.motherduck.region`, since libpq would
otherwise fall back to `PGHOST` or a local socket. The scheme is lowercase, as
libpq's `postgresql://` is.
See [Options](#options) for everything else you can set, including how the
driver finds the token.

The entry point inferred by ADBC driver managers is `AdbcDriverMotherduckInit`,
and the generic `AdbcDriverInit` forwards to it. The original PostgreSQL entry
point `AdbcDriverPostgresqlInit` remains available and does not install the
MotherDuck error-message redaction.

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

The driver also adds `--return_pg_command_tags=on`, so the endpoint answers
DML and DDL with a PostgreSQL command tag that carries the affected-row count
(`INSERT 0 2`, `UPDATE 3`, `DELETE 0`, `CREATE TABLE`) instead of DuckDB's
`Count` or `Success` result set. `ExecuteQuery` on such a statement therefore
returns an empty stream, and `rows_affected` for `UPDATE` and `DELETE` is the
number of rows changed rather than 1. For a statement that returns rows (a
`SELECT`, or DML with `RETURNING`), `rows_affected` is the count in its command
tag. The PostgreSQL driver reports -1 for these when it reads results with
COPY, which MotherDuck doesn't support.

`adbc.motherduck.session_name` (database option) sets MotherDuck's
[session name](https://motherduck.com/docs/sql-reference/connection-string-parameters/#session_name),
which routes connections with the same name to the same replica under read
scaling and appears in query history. The driver adds `--session_name=<name>`
after the URI's session options, escaping whitespace and backslashes with a
backslash; MotherDuck removes the escapes, so `my session` arrives as is. It's
unset by default; an empty value unsets it.

`adbc.motherduck.db_schema` (database option) sets the schema every new
connection starts in. MotherDuck's endpoint doesn't accept `search_path` as a
startup option, so the driver runs `SET search_path TO <database>.<schema>`
once each connection is up, with the connection's current database. (DuckDB
would take a bare name that's also an attached database's name as that
database.) Connecting fails if the schema doesn't exist. It's unset by default, leaving
MotherDuck's default (`main`); an empty value unsets it.
`adbc.connection.db_schema` changes a single connection's schema the same way,
in its current database, and replaces this option when set before `Init`.
Both name a schema only: unlike the PostgreSQL driver's bare `SET search_path`,
which DuckDB would take as an attached database's name when no such schema
exists, a name that isn't a schema of the current database fails with the
server's error. Use `USE <database>` to change databases.

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

## Differences from the PostgreSQL driver

Code written for the ADBC PostgreSQL driver (`adbc_driver_postgresql`) needs
changes to use this driver, and settings for this driver mean nothing to the
PostgreSQL driver:

- **A separate driver.** Load `motherduck`, not `postgresql`. The entry point is
  `AdbcDriverMotherduckInit`, not `AdbcDriverPostgresqlInit`.
- **MotherDuck only.** The driver connects only to MotherDuck's PostgreSQL
  endpoint. Plain PostgreSQL servers aren't supported, because every connection
  sets `attach_mode`.
- **Connection settings.** The MotherDuck token is the password, and the user
  defaults to `postgres`. `PGUSER`, `PGPASSWORD`, `PGAPPNAME`, password files,
  and connection service files are never used, and `sslmode` defaults to
  `require`. The `adbc.motherduck.*` options have no PostgreSQL equivalent.
- **Options that differ.** `adbc.postgresql.use_copy` defaults to `false`, and
  setting it to `true` fails. `adbc.postgresql.type_resolver_mode` is accepted
  and ignored. `adbc.connection.db_schema` names a schema, not an arbitrary
  `search_path`; use `USE <database>` to change databases.
- **Results and errors.** DML and DDL return an empty stream, and `rows_affected`
  for `UPDATE` and `DELETE` is the number of rows changed. Data conversion
  errors map to `INTERNAL` instead of `INVALID_DATA`.
- **Missing features.** Bulk ingest, COPY result reads, statistics, and others
  don't work; see the list below.

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
  `false` (#11); setting it to `true` fails. Results are read from libpq
  instead. A multi-statement script such as `INSERT ...; SELECT 1` stays
  uncommitted, and invisible to other connections, until its rows are read,
  its stream is released, or another command runs on the same connection.
  That wait lets a row that cannot be converted roll the script back.
- **Parameter types:** the endpoint can't prepare a query whose result
  column types depend on its parameters, as in `SELECT $1`, and ignores the
  parameter types sent with the prepare (#17). When that happens with
  parameters bound, the driver prepares the query again with each `$n` cast to
  its bound type, so such queries work once parameters are bound; without
  bound parameters, `GetParameterSchema` and `ExecuteSchema` return
  `NOT_IMPLEMENTED` for them. An all-null parameter whose type the endpoint
  can't infer is cast to `TEXT`, so `SELECT 1 + $1` with a null fails.
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

## Building

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). For how the driver is put together and
how it relates to the upstream PostgreSQL driver, see
[docs/DESIGN.md](docs/DESIGN.md).

## License

The driver is licensed under the Apache License 2.0; see `LICENSE.txt` and
`NOTICE.txt`. Imported ASF headers retain their original text, preceded by a
2026 ADBC Drivers Contributors copyright and modification notice. Other bundled
third-party notices remain intact.

The MotherDuck name and logos (`docs/images/motherduck-logo*.svg`) are
trademarks of MotherDuck, Inc. They are not licensed under the Apache License;
that license grants no trademark rights (section 6). Use them only to identify
MotherDuck, not to imply endorsement of a fork or derived work.
