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
    parser.addoption("--vendor-version", action="store", default="18")


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
    """Give the session a fresh database (see ci/scripts/pre-test.sh) and drop it
    afterwards, so concurrent jobs and earlier runs can't leave tables behind."""
    setup_uri = os.environ.get("MOTHERDUCK_TEST_SETUP_URI")
    catalog = os.environ.get("MOTHERDUCK_TEST_CATALOG")
    if not setup_uri or not catalog:
        yield
        return

    name = driver.quote_one_identifier(catalog)

    def execute(*queries: str) -> None:
        with (
            adbc_driver_manager.dbapi.connect(
                driver=driver_path,
                db_kwargs={**db_kwargs, "uri": setup_uri},
                autocommit=True,
            ) as c,
            c.cursor() as cursor,
        ):
            for query in queries:
                cursor.execute(query)

    execute(f"DROP DATABASE IF EXISTS {name}", f"CREATE DATABASE {name}")
    yield
    execute(f"DROP DATABASE IF EXISTS {name}")


@pytest.fixture(scope="session", autouse=True)
def _setup_backend(request, _motherduck_database, conn_factory) -> None:  # noqa: F811
    with conn_factory() as c, c.cursor() as cursor:
        cursor.execute("CREATE SCHEMA IF NOT EXISTS secondary")
