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

import pytest

from .naming import is_stale_test_name

# The cleanup drops whatever is_stale_test_name accepts, from a shared account,
# so pin down what it accepts. MotherDuckTestNamesTest.IsStaleTestName in
# tests/postgresql_test.cc checks the C++ copy against the same names.
_NOW = 1800000000


@pytest.mark.parametrize(
    "name, stale",
    [
        # A run that's still going, and the schemas and databases named after it.
        ("adbc_test_1799990000_123_local_1_macos_arm64", False),
        ("adbc_test_1799990000_123_local_1_macos_arm64_other_db", False),
        # Interrupted runs, a day or more ago.
        ("adbc_test_1799913600_123_local_1_macos_arm64", True),
        ("adbc_test_1700000000_12_backup", True),
        ("adbc_1700000000_local_1_linux_amd64_99", True),
        ("adbc_1700000000_1_1_test_windows_amd64_7_other_db", True),
        # Everything else stays.
        ("adbc_driver_tests", False),
        ("adbc_1000000000_unrelated", False),
        ("adbc_ci_18473920123_1_test_linux_amd64", False),
        ("adbc_test_", False),
        ("adbc_test_1700000000_12_", False),
        ("my_adbc_test_1700000000_12_x", False),
        ("main", False),
    ],
)
def test_is_stale_test_name(name: str, stale: bool) -> None:
    assert is_stale_test_name(name, _NOW) is stale
