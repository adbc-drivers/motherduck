<!--
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

# MotherDuck ADBC Driver Validation Suite

This directory contains a Python-based validation suite for the MotherDuck ADBC
driver. It runs against MotherDuck's Postgres endpoint; validating against
PostgreSQL itself is not a goal of this repository.

## Testing

You need a MotherDuck token whose account can create and drop databases, and a
build of the driver in `build/`:

```shell
$ pixi run adbc-make build DEBUG=true
$ export MOTHERDUCK_TOKEN=...
```

`ci/scripts/pre-test.sh` writes the connection settings to `.env.override`.
Pass your platform and architecture; they only go into the database name.
Outside GitHub Actions each invocation adds a unique suffix, so this database
never collides with one the C++ tests use.

```shell
$ ./ci/scripts/pre-test.sh macos arm64
$ set -a; source .env.override; set +a
```

Then run the suite from the repository root:

```shell
$ pixi run validate
```

The suite creates a fresh database before it starts and drops it when it
finishes. To use an existing database instead, which the suite leaves in place,
set `MOTHERDUCK_TEST_DATABASE` before running `pre-test.sh`.
`MOTHERDUCK_PG_HOST` overrides the default endpoint,
`pg.us-east-1-aws.motherduck.com`.
