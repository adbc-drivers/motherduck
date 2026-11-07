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

import functools
import os
import re
from pathlib import Path

from adbc_drivers_validation import model, quirks

# MotherDuck says `Table with name foo does not exist!`, or for a failed
# `::regclass` cast, `relation "foo" does not exist`.
_TABLE_NOT_FOUND = re.compile(
    r'(?:table with name|relation) ("*[^\s!]+?"*) does not exist'
)

_database_args = {
    "uri": model.FromEnv("ADBC_POSTGRESQL_TEST_URI"),
}


class PostgreSQLQuirks(model.DriverQuirks):
    name = "postgresql"
    driver = "adbc_driver_postgresql"
    driver_name = "ADBC MotherDuck Driver"
    # The driver still reports PostgreSQL as the vendor name
    # (https://github.com/adbc-drivers/motherduck/issues/15).
    vendor_name = "MotherDuck"
    # It reports no vendor version, rather than the endpoint's PostgreSQL
    # server_version_num.
    vendor_version = ""
    # MotherDuck is a service with no version to pin, so the docs label the
    # results "latest".
    short_version = "latest"
    features = model.DriverFeatures(
        connection_get_table_schema=True,
        connection_transactions=True,
        get_objects=True,
        get_objects_constraints_foreign=False,
        get_objects_constraints_primary=False,
        get_objects_constraints_unique=False,
        metadata_type_name=True,
        # Needs COPY FROM STDIN, which the endpoint lacks (ECO-587).
        statement_bulk_ingest=False,
        statement_bulk_ingest_catalog=False,
        statement_bulk_ingest_schema=False,
        # MotherDuck has no temporary tables.
        statement_bulk_ingest_temporary=False,
        statement_bind=True,
        statement_execute_schema=True,
        statement_get_parameter_schema=True,
        statement_prepare=True,
        statement_rows_affected=True,
        statement_rows_affected_ddl=False,
        # The database ci/scripts/pre-test.sh picks.
        current_catalog=os.environ.get("MOTHERDUCK_TEST_CATALOG", "my_db"),
        current_schema="main",
        secondary_schema="secondary",
        supported_xdbc_fields=["xdbc_type_name"],
    )
    setup = model.DriverSetup(
        database=_database_args,
        connection={},
        statement={},
    )

    @property
    def queries_paths(self) -> tuple[Path, ...]:
        return (Path(__file__).parent.parent / "queries",)

    def bind_parameter(self, index: int) -> str:
        """PostgreSQL uses $1, $2, $3, etc. for parameter placeholders."""
        return f"${index}"

    def is_table_not_found(self, table_name: str | None, error: Exception) -> bool:
        """Check if the error indicates a table not found condition.

        With no table name, any table not found error matches.
        """
        # Match the server's message, not the failed query the driver echoes
        # into the error, which may mention "table" and the table's name.
        match = _TABLE_NOT_FOUND.search(str(error).lower())
        return match is not None and (
            table_name is None or table_name.lower() in match.group(1)
        )

    def quote_one_identifier(self, identifier: str) -> str:
        """Quote an identifier using PostgreSQL's double-quote syntax."""
        identifier = identifier.replace('"', '""')
        return f'"{identifier}"'

    def split_statement(self, statement: str) -> list[str]:
        """Split a multi-statement SQL string into individual statements."""
        return quirks.split_statement(statement)


@functools.cache
def get_quirks(version: str) -> PostgreSQLQuirks:
    quirks = PostgreSQLQuirks()
    if version != quirks.short_version:
        raise ValueError(f"Unsupported MotherDuck version: {version}")
    return quirks
