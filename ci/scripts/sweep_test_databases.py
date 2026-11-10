#!/usr/bin/env python3
# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Drop adbc_ci_* MotherDuck databases left behind by cancelled or crashed runs.

Reads MOTHERDUCK_TOKEN from the environment. MIN_AGE_HOURS (default 24) keeps
databases that a run might still be using; DRY_RUN=true only lists them.
"""

import os
import sys

import duckdb

min_age_hours = float(os.environ.get("MIN_AGE_HOURS") or 24)
dry_run = os.environ.get("DRY_RUN", "false").lower() == "true"

con = duckdb.connect("md:")
stale = con.execute(
    """
    SELECT name FROM md_information_schema.databases
    WHERE starts_with(name, 'adbc_ci_')
      AND created_ts < now() - to_hours(?::BIGINT)
    ORDER BY created_ts
    """,
    [int(min_age_hours)],
).fetchall()

print(f"{len(stale)} stale adbc_ci_* database(s) older than {min_age_hours:g}h")
failed = 0
for (name,) in stale:
    if dry_run:
        print(f"would drop {name}")
        continue
    try:
        con.execute(f'DROP DATABASE IF EXISTS "{name.replace(chr(34), chr(34) * 2)}"')
        print(f"dropped {name}")
    except Exception as e:
        failed += 1
        print(f"::warning::failed to drop {name}: {e}", file=sys.stderr)
sys.exit(1 if failed else 0)
