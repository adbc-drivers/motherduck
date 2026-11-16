<!---
  Copyright (c) 2026 ADBC Drivers Contributors

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-->

# How to Contribute

All contributors are expected to follow the [Code of
Conduct](https://github.com/adbc-drivers/motherduck?tab=coc-ov-file#readme).

The [README](README.md) describes the driver's options, unsupported features,
and code layout. This guide covers how to build, test, and submit a change.

## Reporting Issues and Making Feature Requests

Please file issues and feature requests on the GitHub issue tracker:
https://github.com/adbc-drivers/motherduck/issues

For a bug, include the driver version, your platform, how you connect (the URI
and any driver options), and the SQL or ADBC calls that reproduce it. **Never
paste a MotherDuck token** into an issue, pull request, or log.

Potential security vulnerabilities should be reported to
[security@adbc-drivers.org](mailto:security@adbc-drivers.org) instead.  See
the [Security
Policy](https://github.com/adbc-drivers/motherduck?tab=security-ov-file#readme).

## Where Code Goes

- **MotherDuck behavior goes in `src/motherduck/`**, usually as a subclass of
  the upstream class. Its tests are in `tests/motherduck_*_test.cc`.
- **`src/postgres/` is the upstream PostgreSQL driver**, kept as close to
  Apache Arrow ADBC's as possible so upstream fixes merge cleanly. Change it
  only to add a hook the MotherDuck subclasses need, and keep the change small.
  See [Code layout](docs/DESIGN.md#code-layout) for the differences the fork already
  has. A fix that isn't specific to MotherDuck's endpoint may belong in
  [Apache Arrow ADBC](https://github.com/apache/arrow-adbc) instead.
- **`third_party/`** holds vendored code. Don't edit it here.

Don't add a dependency with a copyleft license (GPL, LGPL, AGPL, or similar);
dependencies must be compatible with the Apache License 2.0. If you add or
update bundled third-party code, update `NOTICE.txt` and `LICENSE.txt` to match.

## Build and Test

### Build

Requires CMake 3.24+, a C++20 compiler, and libpq 15 or newer. The driver bundles the Apache common/framework code, nanoarrow,
fmt, portable-snippets, and boost.int128 it needs. Test builds also require
GoogleTest/GoogleMock; CMake downloads GoogleTest 1.15.2 if it is not installed.

For a native development build, set `PostgreSQL_ROOT` or `CMAKE_PREFIX_PATH`
if CMake cannot find libpq, then run:

```sh
./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64
```

Use `linux` or `windows` and `amd64` or `arm64` as appropriate. With Homebrew,
for example, set `PostgreSQL_ROOT="$(brew --prefix libpq)"` after installing
`libpq`. An existing Homebrew PostgreSQL installation also supplies libpq.

For a release build without test dependencies:

```sh
./ci/scripts/build.sh release macos arm64
```

The scripts copy the library to `build/libadbc_driver_motherduck.dylib` on
macOS, `.so` on Linux, or `.dll` on Windows.

For Foundry packaging, use the vcpkg toolchain through `CMAKE_TOOLCHAIN_FILE`,
`VCPKG_ROOT`, or `VCPKG_INSTALLATION_ROOT`. The scripts choose static vcpkg
triplets, including a static runtime on Windows. A native build with a system
libpq is suitable for local development and retains that runtime dependency.

Direct CMake builds are also supported:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
cmake --install build/debug --prefix build/install
```

### Test

C++ unit tests can be run after building:

```bash
./ci/scripts/test.sh macos arm64
```

On macOS and Windows this runs only the server-independent tests. The
integration tests and the Python validation suite run against a real
MotherDuck account, since plain PostgreSQL isn't supported. Before opening a
pull request that changes driver behavior, run them against MotherDuck
yourself rather than relying on CI. The commands below need
[pixi](https://pixi.prefix.dev/latest/) (`pixi install`); see
[Inherited Tests and Test Isolation](#inherited-tests-and-test-isolation) for the details:

```bash
export MOTHERDUCK_TOKEN=...
./ci/scripts/pre-test.sh macos arm64
set -a
source .env.override
set +a
MOTHERDUCK_TEST_POSTGRESQL=ON ./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64
pixi run validate
```

Test runs create and drop schemas in a shared `adbc_driver_tests` database, so
use a token for an account where that's fine. Don't add tokens or other
credentials to the code, the tests, or the workflow files; CI uses the
repository's own `MOTHERDUCK_TOKEN` secret.

If your change fixes a bug or adds behavior, add a test for it. If it makes a
skipped upstream test pass, or requires skipping one, update
`MOTHERDUCK_SKIPPED_UPSTREAM_TESTS` in `tests/CMakeLists.txt` and the quirks in
`validation/tests/postgresql.py` to match.

The validation suite produces a test report, which can be rendered into a
documentation page (using MyST Markdown):

```shell
$ pixi run gendocs --output generated/
```

Then look at `./generated/motherduck.md`.

### Inherited Tests and Test Isolation

The complete upstream C++ test suites are compiled in test builds. The
integration tests and the Python validation suite run against MotherDuck's
Postgres endpoint; plain PostgreSQL isn't supported, because every connection
sets `attach_mode`. Linux test builds enable the integration tests by default,
and macOS and Windows run only server-independent tests unless you set
`MOTHERDUCK_TEST_POSTGRESQL=ON` when building.

Runs share one database, `adbc_driver_tests`, which the tests create if it's
missing and never drop. Each test process gets its own schema in it,
`adbc_test_<start time>_<pid>_<run>`, where `<run>` is the
`MOTHERDUCK_TEST_RUN` that `ci/scripts/pre-test.sh` writes (the GitHub run,
attempt and job in CI, or `local_<pid>`, followed by the platform and arch). The
start time and process ID keep runs apart even when they source the same
`.env.override`. Every test connection starts in the schema through
`motherduck.db_schema`. Tests that need more schemas or databases name
them after it, and the tests drop all of them when they finish, so concurrent
jobs and runs don't share tables. Each run also drops any schema or database
named `adbc_test_<time>_<pid>_...` (or `adbc_<time>_..._<platform>_<arch>`, as
an earlier version of the tests named them) with a time a day or more ago,
which an interrupted run left behind; only names of those shapes are dropped
(`IsStaleTestName` in `tests/postgresql_test.cc`, mirrored in
`validation/tests/naming.py`, with tests for both). Databases the old
database-per-run setup left, `adbc_ci_<run>_...`, carry no time, so they can't
be told from a live run's and have to be dropped by hand. A database
per run would scale worse: each interrupted run would leave a database and its
retained storage behind, and every workspace-mode connection in the account
attaches every database.

Set `MOTHERDUCK_TEST_DATABASE` to use another database. The tests treat it like
`adbc_driver_tests`: they create it if it's missing, create and drop their
schemas in it, and sweep stale test-named schemas from it and test-named
databases from the account. So the token needs to be able to create databases
and schemas, and a read-only or shared database won't work.
`MOTHERDUCK_PG_HOST` overrides the default host
(`pg.us-east-1-aws.motherduck.com`). Test connections use the driver's default
`attach_mode=single`, so other databases in the account stay out of catalog
queries.

CI does the same, using the `MOTHERDUCK_TOKEN` secret from the `test` GitHub
environment.

The validation suite's quirks in `validation/tests/postgresql.py` describe
MotherDuck: its catalog, the run's schema, no temporary tables, and the vendor
name `MotherDuck`. The driver still reports itself as PostgreSQL (#15), so the
vendor check fails until that's fixed. Published validation documentation will
be configured separately.

## Opening a Pull Request

Before opening a pull request:

- Review your changes and make sure no stray files, etc. are included.
- Ensure the Apache license header is at the top of all files. New files use
  `Copyright (c) 2026 ADBC Drivers Contributors` followed by the Apache License
  2.0 notice, copied from a nearby file of the same type. Files taken from
  Apache Arrow ADBC keep their original ASF header, preceded by the ADBC
  Drivers Contributors copyright and modification notice. Files that can't
  carry a header are listed in `.rat-excludes`.
- Check if there is an existing issue.  If not, please file one, unless the
  change is trivial.
- Assign the issue to yourself by commenting just the word `take`.
- Update the README if you change a driver option, a behavior it describes, or
  its list of unsupported features.
- Run the static checks by installing [pre-commit](https://pre-commit.com/),
  then running `pre-commit run --all-files` from inside the repository.  Make
  sure all your changes are staged/committed (unstaged changes will be
  ignored). This runs clang-format, ruff, codespell, whitespace checks, and an
  Apache RAT license check, which needs Java.

When writing the pull request description:

- Ensure the title follows [Conventional
  Commits](https://www.conventionalcommits.org/en/v1.0.0/) format, which a CI
  check enforces.  The type is one of `build`, `chore`, `ci`, `docs`, `feat`,
  `fix`, `perf`, `refactor`, `revert`, or `test`.  An optional component must
  be a directory path relative to the repo root, e.g. `fix(src): ...` or
  `test(tests): ...`; most titles here leave it out.  Example titles:

  - `feat: add adbc.motherduck.db_schema and qualify the current schema`
  - `fix: strip DuckDB's statement echo from error text`
  - `fix!: return us instead of ms`

  Ensure that breaking changes are appropriately flagged with a `!` as seen
  in the last example above.
- Describe what changed, why, and how you tested it, including whether you ran
  the integration tests against MotherDuck.
- Make sure the description ends with `Closes #NNN`, `Fixes #NNN`, or
  similar, so that the issue will be linked to your pull request.

Pull requests are squash-merged, so the title becomes the commit message on
`main`.

## Releasing

A release is a `v*` git tag pushed to `main`. The tag starts the Custom
Release workflow ([`script_release.yaml`](.github/workflows/script_release.yaml)),
which tests, validates against MotherDuck, builds for linux (amd64, arm64),
macOS (arm64) and Windows (amd64), packages, tests the packages with `dbc`, and
creates a **draft** GitHub release with the packages attached. Nothing is public
until you publish the draft.

1. Make sure `main` is green, then run the validation suite against MotherDuck
   locally (`pixi run validate`, with `MOTHERDUCK_TOKEN` set).
2. Optionally rehearse the workflow: run **Custom Release** manually from the
   Actions tab on `main`
   (`gh workflow run "Custom Release" --ref main`). A manual run does everything
   except create the release.
3. Write the release notes in `docs/release-notes/<tag>.md` and merge them. The
   release tooling generates notes from the commits since the previous stable
   tag, and it ignores pre-release tags. Until the first stable release that is
   the whole history, including the upstream PostgreSQL driver's, so replace the
   generated notes with the hand-written ones.
4. Tag and push. Use SemVer, with a pre-release suffix for alphas and betas
   (`v0.1.0-alpha.1`, not `v0.1.0a1`):

   ```sh
   git checkout main && git pull
   git tag v0.1.0-alpha.1
   git push origin v0.1.0-alpha.1
   ```

5. Wait for the workflow to finish, then open the draft release and check that
   it has the four platform packages, `manifest.yaml`, and the docs page.
6. Replace the generated notes, mark pre-releases, and publish:

   ```sh
   gh release edit v0.1.0-alpha.1 \
     --title "ADBC Driver for MotherDuck v0.1.0-alpha.1" \
     --notes-file docs/release-notes/v0.1.0-alpha.1.md \
     --prerelease
   gh release edit v0.1.0-alpha.1 --draft=false
   ```

   Drop `--prerelease` for a stable release. The release tooling doesn't set it
   for you, and without it GitHub shows the release as the latest one.

If the workflow fails, nothing is published. Fix the problem, delete the tag
(`git tag -d <tag> && git push origin :refs/tags/<tag>`), and tag again, or move
to the next pre-release number. Once a stable tag such as `v0.1.0` exists, the
generated notes for later releases cover only the new commits.

## License

By contributing, you agree that your contributions are licensed under the
[Apache License 2.0](LICENSE.txt).
