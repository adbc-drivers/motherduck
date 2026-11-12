# Copyright (c) 2026 ADBC Drivers Contributors
#
# This file has been modified from its original version, which is
# under the Apache License:
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

import os
import sys
from pathlib import Path

import adbc_driver_manager.dbapi
import adbc_drivers_validation.model
import adbc_drivers_validation.tests.conftest
import pytest
from adbc_drivers_validation.tests.conftest import (  # noqa: F401
    conn,
    conn_factory,
    db_kwargs,
    manual_test,
    noci,
    pytest_collection_modifyitems,
)

from .postgresql import TEST_SCHEMA, get_quirks


def pytest_addoption(parser) -> None:
    adbc_drivers_validation.tests.conftest.pytest_addoption(parser)
    parser.addoption("--vendor-version", action="store", default="latest")


@pytest.fixture(scope="session")
def driver(pytestconfig) -> adbc_drivers_validation.model.DriverQuirks:
    return get_quirks(pytestconfig.getoption("vendor_version"))


@pytest.fixture(scope="session")
def driver_path(driver: adbc_drivers_validation.model.DriverQuirks) -> str:
    ext = {
        "win32": "dll",
        "darwin": "dylib",
    }.get(sys.platform, "so")
    base = Path(__file__).resolve().parent.parent.parent
    return str(base / f"build/libadbc_driver_motherduck.{ext}")


# The name of a run's schema, or of a schema or database named after it, with
# the run's Unix time as group 1 (see ci/scripts/pre-test.sh). The full shape
# keeps the sweep from touching anything else in the account.
_TEST_NAME_PATTERN = "^adbc_([0-9]{10})_.+_(linux|macos|windows)_(amd64|arm64)(_.+)?$"


def _stale(column: str) -> str:
    """'stale' for a test-shaped name in column with a time a day or more ago."""
    return (
        f"CASE WHEN TRY_CAST(regexp_extract({column}, '{_TEST_NAME_PATTERN}', 1) "
        "AS BIGINT) < CAST(epoch(now()) AS BIGINT) - 86400 THEN 'stale' ELSE '' END"
    )


@pytest.fixture(scope="session")
def _motherduck_database(driver, driver_path, db_kwargs):  # noqa: F811
    """Give the session a schema of its own in the shared test database (see
    ci/scripts/pre-test.sh), and afterwards drop it, the other schemas and the
    databases named after it, and whatever runs left behind a day or more ago,
    so concurrent jobs and interrupted runs can't leave tables behind."""
    setup_uri = os.environ.get("MOTHERDUCK_TEST_SETUP_URI")
    catalog = os.environ.get("MOTHERDUCK_TEST_CATALOG")
    schema = TEST_SCHEMA
    if not setup_uri or not catalog or not schema:
        yield
        return

    quote = driver.quote_one_identifier
    database = quote(catalog)
    # The setup connection is to md:, where the run's schema doesn't resolve.
    setup_kwargs = {
        key: value
        for key, value in db_kwargs.items()
        if key != "adbc.motherduck.db_schema"
    }

    def connect():
        return adbc_driver_manager.dbapi.connect(
            driver=driver_path,
            # Single attach mode can't connect to md:.
            db_kwargs={
                **setup_kwargs,
                "uri": setup_uri,
                "adbc.motherduck.attach_mode": "workspace",
            },
            autocommit=True,
        )

    def leftover(name: str, stale: str) -> bool:
        return name == schema or name.startswith(schema + "_") or stale == "stale"

    def drop_leftovers(cursor) -> None:
        cursor.execute(
            f"SELECT schema_name, {_stale('schema_name')} "
            "FROM duckdb_schemas() WHERE database_name = current_database()"
        )
        for name, stale in cursor.fetchall():
            if leftover(name, stale):
                cursor.execute(
                    f"DROP SCHEMA IF EXISTS {database}.{quote(name)} CASCADE"
                )
        cursor.execute(
            f"SELECT database_name, {_stale('database_name')} FROM duckdb_databases()"
        )
        for name, stale in cursor.fetchall():
            if name != catalog and leftover(name, stale):
                cursor.execute(f"DROP DATABASE IF EXISTS {quote(name)}")

    with connect() as c, c.cursor() as cursor:
        create = f"CREATE DATABASE IF NOT EXISTS {database}"
        try:
            cursor.execute(create)
        except adbc_driver_manager.dbapi.Error:
            # Runs that start together race to create it the first time. The
            # loser fails, but by then the database exists.
            cursor.execute(create)
        cursor.execute(f"USE {database}")
        drop_leftovers(cursor)
        cursor.execute(f"CREATE SCHEMA {quote(schema)}")
    yield
    with connect() as c, c.cursor() as cursor:
        cursor.execute(f"USE {database}")
        drop_leftovers(cursor)


@pytest.fixture(scope="session", autouse=True)
def _setup_backend(request, driver, _motherduck_database, conn_factory) -> None:  # noqa: F811
    secondary = driver.quote_one_identifier(driver.features.secondary_schema)
    with conn_factory() as c, c.cursor() as cursor:
        cursor.execute(f"CREATE SCHEMA IF NOT EXISTS {secondary}")
