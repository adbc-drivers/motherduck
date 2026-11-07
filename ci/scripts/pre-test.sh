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
# By default each job gets a fresh database, which the tests create and drop, so
# concurrent jobs and runs don't share tables. An explicit database is left as is.
if [[ -n "${MOTHERDUCK_TEST_DATABASE:-}" ]]; then
  database="$MOTHERDUCK_TEST_DATABASE"
  # The driver's default single attach mode needs a named database.
  if [[ "$database" == "md:" ]]; then
    printf 'MOTHERDUCK_TEST_DATABASE must name a database, not md:\n' >&2
    exit 1
  fi
  manage_database=false
else
  run="${GITHUB_RUN_ID:-local}_${GITHUB_RUN_ATTEMPT:-1}"
  database="adbc_ci_${run}_${GITHUB_JOB:-test}_${1}_${2}"
  manage_database=true
fi

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
  printf 'postgresql://postgres@%s:5432/%s?sslmode=verify-full&sslrootcert=%s' \
    "$host" "${1//:/%3A}" "$ca_bundle"
}

# The driver reads the token from MOTHERDUCK_TOKEN, which keeps it out of the
# URI and test output.
{
  printf 'ADBC_POSTGRESQL_TEST_URI=%q\n' "$(uri_for "$database")"
  printf 'MOTHERDUCK_TEST_CATALOG=%q\n' "$database"
  if [[ "$manage_database" == true ]]; then
    # The tests create and drop the database through this connection.
    printf 'MOTHERDUCK_TEST_SETUP_URI=%q\n' "$(uri_for md:)"
  fi
} > "${repo_root}/.env.override"
