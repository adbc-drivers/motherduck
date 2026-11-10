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

import adbc_drivers_validation.tests.connection
import pytest
from adbc_drivers_validation.tests.connection import generate_tests

from . import postgresql


class TestConnection(adbc_drivers_validation.tests.connection.TestConnection):
    @pytest.fixture(scope="class")
    @classmethod
    def get_objects_table(cls, driver, conn):
        # It ingests, which needs COPY FROM STDIN (#31).
        pytest.skip("Needs COPY FROM STDIN (#31)")

    def test_get_objects_table_types_ignore_case(self, driver, conn) -> None:
        """The table types filter ignores case and takes BASE TABLE for table."""
        with conn.cursor() as cursor:
            cursor.execute("DROP VIEW IF EXISTS mdv_types_v")
            cursor.execute("CREATE OR REPLACE TABLE mdv_types_t (id INT)")
            cursor.execute("CREATE VIEW mdv_types_v AS SELECT 1 AS x")
        cases = [
            (["TABLE"], {"mdv_types_t"}),
            (["BASE TABLE"], {"mdv_types_t"}),
            (["View"], {"mdv_types_v"}),
            (["table", "VIEW"], {"mdv_types_t", "mdv_types_v"}),
        ]
        for table_types, expected in cases:
            objects = (
                conn.adbc_get_objects(
                    depth="tables",
                    catalog_filter=driver.features.current_catalog,
                    table_name_filter="mdv_types_%",
                    table_types_filter=table_types,
                )
                .read_all()
                .to_pylist()
            )
            found = {
                table["table_name"]
                for catalog in objects
                for schema in catalog["catalog_db_schemas"] or []
                for table in schema["db_schema_tables"] or []
            }
            assert found == expected, table_types

    def test_get_objects_temp_view(self, driver, conn) -> None:
        """Temporary views are listed under the temp catalog."""
        with conn.cursor() as cursor:
            cursor.execute("CREATE OR REPLACE TEMP VIEW mdv_temp_v AS SELECT 1 AS x")
        objects = (
            conn.adbc_get_objects(
                depth="columns", catalog_filter="temp", table_name_filter="mdv_temp_v"
            )
            .read_all()
            .to_pylist()
        )
        tables = [
            (catalog["catalog_name"], schema["db_schema_name"], table)
            for catalog in objects
            for schema in catalog["catalog_db_schemas"] or []
            for table in schema["db_schema_tables"] or []
        ]
        assert [(c, s, t["table_name"], t["table_type"]) for c, s, t in tables] == [
            ("temp", "main", "mdv_temp_v", "view")
        ]
        assert [col["column_name"] for col in tables[0][2]["table_columns"]] == ["x"]
        schema = conn.adbc_get_table_schema("mdv_temp_v", catalog_filter="temp")
        assert schema.names == ["x"]


def pytest_generate_tests(metafunc) -> None:
    return generate_tests(
        [postgresql.get_quirks(metafunc.config.getoption("vendor_version"))], metafunc
    )
