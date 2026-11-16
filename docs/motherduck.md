---
# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
{}
---

{{ cross_reference|safe }}
# MotherDuck Driver {{ version }}

{{ heading|safe }}

This driver provides ADBC access to MotherDuck.

:::{warning}
This driver is **not** a drop-in replacement for the ADBC PostgreSQL driver,
although it is built from it and talks to MotherDuck's PostgreSQL protocol
endpoint. It is a separate driver with its own name, it only works with
MotherDuck, and some of the PostgreSQL driver's options and features behave
differently or don't work. See "Differences from the PostgreSQL Driver" below.
:::

## Installation & Quickstart

The driver can be installed with [dbc](https://docs.columnar.tech/dbc):

```bash
dbc install --pre motherduck
```

## Connecting

The driver connects to MotherDuck's PostgreSQL protocol endpoint and uses your
MotherDuck token as the password. Set the token in the `MOTHERDUCK_TOKEN`
environment variable, which keeps it out of connection strings and logs, and
give the database to connect to and your account's region. Find your region
with `md_user_info()`.

```python
from adbc_driver_manager import dbapi

with dbapi.connect(
    driver="motherduck",
    db_kwargs={
        "uri": "motherduck:///my_db",
        "adbc.motherduck.region": "us-east-1",
    },
) as conn:
    with conn.cursor() as cursor:
        cursor.execute("SELECT 42")
        print(cursor.fetch_arrow_table())
```

The region sets the host, so the URI can leave it out. `us-east-1` connects to
`pg.us-east-1-aws.motherduck.com`. Without the option, put the host in the URI
instead: `motherduck://pg.us-east-1-aws.motherduck.com/my_db`.

The URI's scheme names the driver, so an ADBC driver manager can pick this
driver from a `motherduck://` URI alone. Pass the URI in place of the driver
name:

```python
from adbc_driver_manager import dbapi

with dbapi.connect(
    "motherduck:///my_db",
    db_kwargs={"adbc.motherduck.region": "us-east-1"},
) as conn:
    ...
```

A driver manager matches the scheme against the driver's name, so it doesn't
find the driver from an `md://` URI. When you use `md://`, name the driver as
well, with `driver="motherduck"`.

By default the driver connects in `single` attach mode, which attaches only the
database named in the connection. To work across every database in the account,
connect to `md:` and set `adbc.motherduck.attach_mode` to `workspace`.

### Connection String Format

The URI is a `motherduck://` URI:

```text
motherduck://[USER[:TOKEN]@][HOST[:PORT]]/DATABASE[?PARAMETER=VALUE&...]
```

`md://` is accepted as a short form of the scheme, with the same syntax. The
driver accepts no other scheme, and the scheme is lowercase.

Components:

- `DATABASE`: the MotherDuck database to connect to, or `md:` (written `md%3A`
  in a URI) in `workspace` attach mode.
- `HOST` and `PORT`: the endpoint, such as `pg.us-east-1-aws.motherduck.com`.
  Optional when `adbc.motherduck.region` is set; the port is then 5432 unless
  the URI sets one.
- `USER`: optional. The default is `postgres`, which MotherDuck expects.
- `TOKEN`: the MotherDuck token. Prefer `MOTHERDUCK_TOKEN` or a database option
  to putting it in the URI.

Other URI parameters are PostgreSQL settings, which aren't supported; see below.

### PostgreSQL settings are not supported

The driver reaches MotherDuck through a PostgreSQL client library, which has
settings of its own. None of them are part of this driver's supported
interface, and any of them may stop working, or work differently, in a future
version, including a version that no longer uses that library:

- URI parameters, such as `sslmode`, `sslrootcert`, `options` and `service`.
- PostgreSQL environment variables, such as `PGHOST`, `PGPORT`, `PGSSLMODE`,
  `PGSSLROOTCERT` and `PGOPTIONS`, and files such as `~/.pg_service.conf`.
  Some are read today and some are ignored, as described under
  [Differences from the PostgreSQL Driver](#differences-from-the-postgresql-driver).

Use the `adbc.motherduck.*` options and the standard ADBC options instead.
The driver has no supported option for certificate verification yet.
`sslmode=verify-full` with `sslrootcert` works today, but it is one of the
unsupported settings.

### Authentication

The driver takes the token from the first of these that is set:

1. The `password` database option, or its alias `adbc.motherduck.token`.
2. The password in the URI.
3. The `MOTHERDUCK_TOKEN` environment variable.

Without a token, connecting fails with `UNAUTHENTICATED`.

### Driver Options

| Option | Level | Description |
| --- | --- | --- |
| `adbc.motherduck.attach_mode` | Database | `single` (default) attaches only the named database; `workspace` attaches every database in the account. |
| `adbc.motherduck.region` | Database | The MotherDuck region, such as `us-east-1` or `eastus2`, which sets the host. |
| `adbc.motherduck.session_name` | Database | MotherDuck's [session name](https://motherduck.com/docs/sql-reference/connection-string-parameters/#session_name), which routes connections with the same name to the same replica under read scaling. |
| `adbc.motherduck.application_name` | Database | Your application's name. The driver adds `adbc_driver_motherduck/<version>` after it, so it appears in MotherDuck's query history. |
| `adbc.motherduck.connect_timeout` | Database | The longest time to wait while connecting, in whole seconds. `0` (the default) waits indefinitely; the shortest timeout is 2 seconds, so `1` acts as `2`. |
| `adbc.motherduck.db_schema` | Database | The schema every new connection starts in. Connecting fails if it doesn't exist. |
| `adbc.connection.db_schema` | Connection | Changes a single connection's schema, in its current database. |
| `adbc.motherduck.token` | Database | Alias for the standard `password` option. |

## Differences from the PostgreSQL Driver

Code written for the ADBC PostgreSQL driver (`adbc_driver_postgresql`) needs
changes to use this driver, and settings for this driver mean nothing to the
PostgreSQL driver:

- **A separate driver.** Load `motherduck`, not `postgresql`.
- **MotherDuck only.** The driver connects only to MotherDuck's PostgreSQL
  endpoint. Plain PostgreSQL servers aren't supported, because every connection
  sets `attach_mode`.
- **Connection URI.** The URI must use the `motherduck://` (or `md://`) scheme,
  not `postgresql://`.
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
  don't work; the feature table below shows what is supported.

## Feature & Type Support

{{ features|safe }}

### Types

{{ types|safe }}

{{ footnotes|safe }}

## Compatibility

{{ compatibility_info|safe }}
