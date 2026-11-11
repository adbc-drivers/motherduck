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

from .postgresql import get_quirks


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


@pytest.fixture(scope="session")
def _motherduck_database(driver, driver_path, db_kwargs):  # noqa: F811
    """Give the session a schema of its own in the shared test database (see
    ci/scripts/pre-test.sh), and drop it and every schema named after it
    afterwards, so concurrent jobs and earlier runs can't leave tables behind."""
    setup_uri = os.environ.get("MOTHERDUCK_TEST_SETUP_URI")
    catalog = os.environ.get("MOTHERDUCK_TEST_CATALOG")
    schema = os.environ.get("MOTHERDUCK_TEST_SCHEMA")
    if not setup_uri or not catalog or not schema:
        yield
        return

    database = driver.quote_one_identifier(catalog)
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

    def drop_run_schemas() -> None:
        with connect() as c, c.cursor() as cursor:
            cursor.execute(
                "SELECT schema_name FROM duckdb_schemas() WHERE database_name = $1 "
                "AND (schema_name = $2 OR starts_with(schema_name, $3))",
                (catalog, schema, schema + "_"),
            )
            names = [row[0] for row in cursor.fetchall()]
            for name in names:
                cursor.execute(
                    f"DROP SCHEMA IF EXISTS {database}."
                    f"{driver.quote_one_identifier(name)} CASCADE"
                )

    with connect() as c, c.cursor() as cursor:
        cursor.execute(f"CREATE DATABASE IF NOT EXISTS {database}")
    drop_run_schemas()
    with connect() as c, c.cursor() as cursor:
        cursor.execute(
            f"CREATE SCHEMA {database}.{driver.quote_one_identifier(schema)}"
        )
    yield
    drop_run_schemas()


@pytest.fixture(scope="session", autouse=True)
def _setup_backend(request, driver, _motherduck_database, conn_factory) -> None:  # noqa: F811
    secondary = driver.quote_one_identifier(driver.features.secondary_schema)
    with conn_factory() as c, c.cursor() as cursor:
        cursor.execute(f"CREATE SCHEMA IF NOT EXISTS {secondary}")
