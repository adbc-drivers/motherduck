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
        "uri": "motherduck:///my_db",
        "motherduck.region": "us-east-1",
    },
) as conn:
    with conn.cursor() as cursor:
        cursor.execute("SELECT 42")
        print(cursor.fetch_arrow_table())
```

The region sets the host, so the URI can leave it out: `us-east-1` connects to
`pg.us-east-1-aws.motherduck.com`. Without the option, put the host in the URI,
as in `motherduck://pg.us-east-1-aws.motherduck.com/my_db`. To use a driver you
built yourself, pass the path to the library as `driver`, such as
`build/libadbc_driver_motherduck.dylib`.

The URI is a `motherduck://` URI:
`motherduck://[user[:token]@][host][:port]/database[?parameters]`. Two short
forms are accepted: `md://` with the same syntax, and DuckDB's
`md:[database][?parameters]`, which has no host and so needs
`motherduck.region`; `md:` alone is the workspace. The `md:` form's database
name can't contain `/` or `@`, so DuckDB's `md:<token>@<database>` is rejected;
put the token in `MOTHERDUCK_TOKEN` or the `password` option. The
driver accepts no other scheme, and the scheme is lowercase.

The query takes the `motherduck.*` database options by name, such as
`motherduck:///my_db?motherduck.region=us-east-1&motherduck.connect_timeout=10`
(percent-encode values; an empty value is unset). Every option below can be set
this way except `motherduck.token`, so a token doesn't end up in a query, which
is more likely to be logged than the URI's password. An option set on the
database takes precedence over the URI's. Any other query parameter, including
the [PostgreSQL settings](#postgresql-settings-are-not-supported) such as
`sslmode`, is rejected with `INVALID_ARGUMENT`, and the driver never passes the
query to libpq. Write a `?` in a password as `%3F`. An unencoded one is read as part of the
password only when the text before it can't be a host and port, as in
`user:pass?word@host`; in `host?motherduck.session_name=a@b` the `?` starts the
query, so the `@` stays in the option's value. A port that isn't a number is rejected.

A `motherduck://` URI passed in place of a driver name, as in
`dbapi.connect("motherduck:///my_db")`, lets an ADBC driver manager load this
driver from its scheme. The `md` forms don't, because the scheme has to match the driver's
name, so name the driver as well: `driver="motherduck"`.
See [Options](#options) for everything else you can set, including how the
driver finds the token.

The entry point inferred by ADBC driver managers is `AdbcDriverMotherduckInit`,
and the generic `AdbcDriverInit` forwards to it. The original PostgreSQL entry
point `AdbcDriverPostgresqlInit` remains available and does not install the
MotherDuck error-message redaction.

## Options

`motherduck.attach_mode` (database option) sets the MotherDuck
[attach mode](https://motherduck.com/docs/sql-reference/postgres-endpoint/#session-options):
`single` (the default) attaches only the database named in the connection,
and `workspace` attaches every database in the account. The driver adds
`--attach_mode=<mode>` to libpq's `options`. MotherDuck keeps the last value
of a repeated setting, so the default goes before any session options from
`PGOPTIONS`, which can override it, and an explicitly set option goes
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

`motherduck.session_name` (database option) sets MotherDuck's
[session name](https://motherduck.com/docs/sql-reference/connection-string-parameters/#session_name),
which routes connections with the same name to the same replica under read
scaling and appears in query history. The driver adds `--session_name=<name>`
after any `PGOPTIONS`, escaping whitespace and backslashes with a
backslash; MotherDuck removes the escapes, so `my session` arrives as is. It's
unset by default; an empty value unsets it.

`motherduck.db_schema` (database option) sets the schema every new
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

`motherduck.application_name` (database option) sets your application's
name. MotherDuck builds the user agent in query history and its logs from it,
and the driver adds `adbc_driver_motherduck/<version>` after the name, so
`dbt` becomes `dbt adbc_driver_motherduck/<version>`. PostgreSQL cuts names at
63 bytes, so the driver shortens a long name to keep its own identifier whole.
It's unset by default, which leaves just the driver's own; an empty value unsets
it. `PGAPPNAME` is never used.

`motherduck.connect_timeout` (database option) is the longest time to wait
while connecting, as a whole number of seconds. `0` waits indefinitely, and the
shortest timeout is 2 seconds, so `1` acts as `2`. It's unset by default, which
sets no timeout of its own; an empty value unsets it, and a value that isn't a
whole number fails with `INVALID_ARGUMENT`. Languages whose driver manager sends
an integer, such as Python's `10`, can pass one: the option also works through
the integer setter and getter.

The MotherDuck token is the connection's password. The driver takes it from,
in order: the standard ADBC `password` database option (or its alias,
`motherduck.token`), a password in the URI, and the `MOTHERDUCK_TOKEN`
environment variable. It always passes the token to libpq, so `PGPASSWORD` and
password files (`~/.pgpass`, `PGPASSFILE`) are never used. Without a token,
connecting fails with `UNAUTHENTICATED`.

The user is `postgres`, which MotherDuck's endpoint expects, so the URI can
leave it out. The standard ADBC `username` database option, then a user in the
URI, replace it. `PGUSER` is never used. Reading the `username` option returns
the user the driver connects as.

`motherduck.region` (database option) sets the host from a MotherDuck
region, so the URI can leave it out: `us-east-1` connects to
`pg.us-east-1-aws.motherduck.com`, and `eastus2` connects to
`pg.eastus2-azure.motherduck.com`. Find your region with `md_user_info()`. A
region is dash-separated parts of letters and digits, which the driver
lowercases. One with
dashes is taken to be on AWS and one without on Azure, unless it ends in
`-aws` or `-azure`. The driver doesn't check the region against a list, so a
mistyped one fails at connect with a DNS error naming the host. Setting it
together with a different host in the URI, with `PGHOSTADDR`, or with a
connection service (`PGSERVICE`, which can set `hostaddr`) is an error; a host that matches the region, ignoring case
and a trailing dot, is allowed. When it's set, `PGHOST` and `PGPORT` are
ignored, and the port is 5432 unless the URI sets one. For example, with
`region` set to `us-east-1`:

```text
motherduck:///my_db
```

The driver verifies the server's certificate: unless `PGSSLMODE` sets
`sslmode`, it uses `sslmode=verify-full` instead of libpq's default,
`prefer`, which can send the token unencrypted. The driver's libpq can't read
the OS's trusted certificates itself, so unless the connection names root
certificates, the driver passes the first it finds of these as `sslrootcert`:

1. The file `SSL_CERT_FILE` names. If it can't be read, connecting fails
   instead of trying the others.
2. On Linux, the distribution's CA bundle: `/etc/ssl/certs/ca-certificates.crt`,
   `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, or
   `/etc/ssl/cert.pem`.
3. On macOS and Windows, the OS trust store (the keychains' trust settings, or
   the Windows `ROOT` store), which the driver exports once per process to
   `adbc_driver_motherduck_roots.pem` in the user's cache directory (macOS) or
   temporary directory (Windows).

If the driver finds no root certificates, connecting fails rather than falling
back to a connection that doesn't verify the server, which would send the token
to whoever answers. This happens in minimal container images without the
`ca-certificates` package; install it, or set one of the options below.

Most connections need no TLS settings: on macOS, Windows, and Linux with the
OS's CA certificates installed, the driver verifies the server on its own. Two
optional database options (which a URI's query can also set) cover the other
cases:

- `motherduck.tls_root_cert_path` (optional): a PEM file of root
  certificates to verify the server with, instead of the ones the driver
  finds, such as a corporate CA bundle. The driver
  uses `sslmode=verify-full`, and the connection fails if the file can't be
  read. It outranks `PGSSLMODE`, `PGSSLROOTCERT` and a connection service
  file.
- `motherduck.tls_verify` (optional): `false` encrypts the connection without
  verifying the server's certificate (`sslmode=require`), and doesn't need
  root certificates. Defaults to `true`. It outranks `PGSSLMODE` and a connection
  service file, but libpq still checks the server's CA when `PGSSLROOTCERT`
  or its default file (`~/.postgresql/root.crt`) names root certificates.

Setting both is an error. An empty `tls_verify` unsets it, like the other
options.

## Browser login

`motherduck.auth_type` chooses how the driver gets the MotherDuck token it
sends as the password: `token` (the default) takes it as described under
[Options](#options), and `browser` logs in through the browser, so
organizations that enforce SSO get their identity provider's login page. With
`browser`, set no token (no `password` or `motherduck.token` option, and no
password in the URI; `MOTHERDUCK_TOKEN` is ignored). The URI may leave out the
host and the region, as in `md:my_db?motherduck.auth_type=browser`.

`AdbcDatabaseInit` logs in if needed: it prints a URL and code to stderr,
opens the browser, and blocks until the login is approved in the browser, for
at most `motherduck.login_timeout`. It doesn't open a browser on Linux without
`DISPLAY` or `WAYLAND_DISPLAY`; open the printed URL on any device instead.
Like the MotherDuck DuckDB extension's login, it doesn't use an HTTP proxy.

- The login gives a short-lived MotherDuck token, valid for 24 hours; after
  that, the next connection logs in again.
- The token is saved to `~/.motherduck/credentials.json` (or
  `$MOTHERDUCK_HOME/credentials.json`, which must be absolute), readable only
  by the user, and a saved token is used without opening a browser. The driver
  reads and writes only the file's top-level `motherduck_token` (in a
  version 1 file) and keeps everything else in it, such as other tools' saved
  profiles; a rejected token is removed from the file, not the whole file. The
  driver refuses to use a credentials directory that other users can write to.
- Processes that use the same file take turns logging in, through
  `credentials.json.lock` next to it, so they share one login. The process
  logging in writes its process ID and when its login ends at the latest to
  `credentials.json.login`. The others wait for that login as long as it takes,
  and give up once it has been overdue for 10 seconds, naming the stuck
  process. They wait at most `login_timeout` (or 30 seconds with `0`) for a
  process that holds the lock without logging in.
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
- Without a host from the URI, `PGHOST`, `PGHOSTADDR`, `motherduck.region` or
  a connection service, the driver connects to the Postgres endpoint of the
  user's organization, which it looks up at
  `https://api.motherduck.com/mom/lookup-user-with-token`.
- If the endpoint rejects a saved token and the MotherDuck API confirms that
  it expired or was revoked, the driver removes it and logs in again, once,
  both in `AdbcDatabaseInit` and in `AdbcConnectionInit`.

Related options and environment variables:

| Name | Purpose |
| --- | --- |
| `motherduck.auth_type` (database option) | `token` (default) or `browser`. |
| `motherduck.login_timeout` (database option) | Seconds to wait for the login (default `120`, at most `999999999`); `0` waits until the code expires, after 15 minutes. |
| `motherduck.oauth_client_id` (database option) | The Auth0 application to log in through. |
| `motherduck_host` (environment) | The MotherDuck API host, for example `api.staging.motherduck.com`. |
| `motherduck.tls_root_cert_path` (database option) or `PGSSLROOTCERT` (environment) | A CA bundle that the login's HTTPS requests trust as well as the system's trust store. One that can't be read is skipped for the login. |

Like the other options, these can be set in the URI's query, and they're read
when the database is initialized: changing them afterwards has no effect.

## PostgreSQL settings are not supported

The driver reaches MotherDuck through a PostgreSQL client library, which has
settings of its own. None of them are part of this driver's supported
interface, and any of them may stop working, or work differently, in a future
version, including a version that no longer uses that library:

- URI query parameters, such as `sslmode`, `sslrootcert`, `options` and
  `service`. These are rejected: the URI's query takes only the
  `motherduck.*` options.
- PostgreSQL environment variables, such as `PGHOST`, `PGPORT`, `PGSSLMODE`,
  `PGSSLROOTCERT`, `PGOPTIONS` and `PGSERVICE`, and files such as
  `~/.pg_service.conf`. Some are read today and some are ignored, as described
  under [Options](#options) and
  [Differences from the PostgreSQL driver](#differences-from-the-postgresql-driver).
  Connection service files aren't supported: the driver always passes
  `options`, which replaces any `options` a service file sets, doesn't default
  `sslmode` or `sslrootcert` when one is named, and can't combine one with a
  `region`.

Use the `motherduck.*` options and the standard ADBC options instead. Where the
sections above and below describe one of these settings, they describe today's
behavior, not a commitment.

## Differences from the PostgreSQL driver

Code written for the ADBC PostgreSQL driver (`adbc_driver_postgresql`) needs
changes to use this driver, and settings for this driver mean nothing to the
PostgreSQL driver:

- **A separate driver.** Load `motherduck`, not `postgresql`. The entry point is
  `AdbcDriverMotherduckInit`, not `AdbcDriverPostgresqlInit`.
- **MotherDuck only.** The driver connects only to MotherDuck's PostgreSQL
  endpoint. Plain PostgreSQL servers aren't supported, because every connection
  sets `attach_mode`.
- **Connection URI.** The URI must use the `motherduck://` scheme (or `md://`, or `md:`),
  not `postgresql://`.
- **Connection settings.** The MotherDuck token is the password, and the user
  defaults to `postgres`. `PGUSER`, `PGPASSWORD`, `PGAPPNAME`, password files,
  and connection service files are never used, and `sslmode` defaults to
  `verify-full`. The `motherduck.*` options have no PostgreSQL equivalent.
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
