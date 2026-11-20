#!/usr/bin/env bash
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

# Writes .env.override, which the workflow sources before running test.sh or
# the validation suite.

set -euo pipefail
if [[ $# -lt 2 ]]; then
  printf 'usage: %s <linux|macos|windows> <amd64|arm64> [service]\n' "$0" >&2
  exit 2
fi
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

if [[ -z "${MOTHERDUCK_TOKEN:-}" ]]; then
  printf 'MOTHERDUCK_TOKEN must be set to run tests against MotherDuck\n' >&2
  exit 1
fi

host="${MOTHERDUCK_PG_HOST:-pg.us-east-1-aws.motherduck.com}"
# Runs share one database, which is created if missing and never dropped, and
# each run gets its own schema in it, which the tests create and drop. Creating
# and dropping a database per run would leave a database (and its retained
# storage) behind for every interrupted run, and every workspace-mode
# connection in the account attaches each one.
database="${MOTHERDUCK_TEST_DATABASE:-adbc_driver_tests}"
# The driver's default single attach mode needs a named database.
if [[ "$database" == "md:" ]]; then
  printf 'MOTHERDUCK_TEST_DATABASE must name a database, not md:\n' >&2
  exit 1
fi
if [[ -n "${GITHUB_RUN_ID:-}" ]]; then
  run="${GITHUB_RUN_ID}_${GITHUB_RUN_ATTEMPT:-1}_${GITHUB_JOB:-test}"
else
  # Local runs need their own schema too, so concurrent runs don't collide.
  run="local_$$"
fi
# Each test process names its schema after this, with its start time and
# process ID (see RunSchema in tests/postgresql_test.cc).
test_run="${run}_${1}_${2}"

# vcpkg's libpq can't see the OS trust store, so use the pixi env's CA bundle.
# The generated test job doesn't install the env itself.
if [[ ! -d "${repo_root}/.pixi/envs/default" ]] && command -v pixi >/dev/null 2>&1; then
  (cd "$repo_root" && pixi install)
fi
ca_bundle=""
for candidate in \
  "${repo_root}/.pixi/envs/default/ssl/cacert.pem" \
  "${repo_root}/.pixi/envs/default/Library/ssl/cacert.pem"; do
  if [[ -f "$candidate" ]]; then
    ca_bundle="$candidate"
    break
  fi
done
if [[ -z "$ca_bundle" ]]; then
  printf 'CA bundle not found; run pixi install first\n' >&2
  exit 1
fi
if command -v cygpath >/dev/null 2>&1; then
  ca_bundle="$(cygpath -m "$ca_bundle")"
fi

uri_for() {
  printf 'motherduck://postgres@%s:5432/%s?motherduck.tls_root_cert_path=%s' \
    "$host" "${1//:/%3A}" "$ca_bundle"
}

# The driver reads the token from MOTHERDUCK_TOKEN, which keeps it out of the
# URI and test output.
{
  printf 'ADBC_POSTGRESQL_TEST_URI=%q\n' "$(uri_for "$database")"
  printf 'MOTHERDUCK_TEST_CATALOG=%q\n' "$database"
  printf 'MOTHERDUCK_TEST_RUN=%q\n' "$test_run"
  # The tests create the database and their schema through this connection.
  printf 'MOTHERDUCK_TEST_SETUP_URI=%q\n' "$(uri_for md:)"
} > "${repo_root}/.env.override"
