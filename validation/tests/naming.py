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

"""Names of the test runs' schemas and databases in the shared test database."""

import re

# Runs name their schemas, and the schemas and databases named after them,
# adbc_test_<time>_<pid>_... (TEST_SCHEMA in postgresql.py); earlier versions
# of the tests named them adbc_<time>_..._<os>_<arch>[_...].
_CURRENT = re.compile(r"adbc_test_([0-9]{10})_[0-9]+_.+")
_EARLIER = re.compile(r"adbc_([0-9]{10})_.+_(linux|macos|windows)_(amd64|arm64)(_.+)?")


def is_stale_test_name(name: str, now: int) -> bool:
    """Whether name is a test run's schema or database, or one named after it,
    from a run that started a day or more before now (a Unix time): one an
    interrupted run left behind.

    Databases from the database-per-run setup, adbc_ci_<run>_..., carry no
    time, so they can't be told apart from a live run's and are left alone.
    IsStaleTestName in tests/postgresql_test.cc has the same rule; keep the
    two, and their tests, in step.
    """
    match = _CURRENT.fullmatch(name) or _EARLIER.fullmatch(name)
    return match is not None and int(match.group(1)) <= now - 86400
