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
  See [Code layout](README.md#code-layout) for the differences the fork already
  has. A fix that isn't specific to MotherDuck's endpoint may belong in
  [Apache Arrow ADBC](https://github.com/apache/arrow-adbc) instead.
- **`third_party/`** holds vendored code. Don't edit it here.

Don't add a dependency with a copyleft license (GPL, LGPL, AGPL, or similar);
dependencies must be compatible with the Apache License 2.0. If you add or
update bundled third-party code, update `NOTICE.txt` and `LICENSE.txt` to match.

## Build and Test

### Build

You need CMake 3.24+, a C++20 compiler, and libpq 15 or newer. Build the
driver by passing the configuration, platform, and architecture to the CI build
script:

```bash
./ci/scripts/build.sh [test|release] [linux|windows|macos] [amd64|arm64]
```

The [README's Build section](README.md#build) explains how to point CMake at
libpq, the vcpkg toolchain used for packaging, and direct CMake builds. The
script builds under `build/ci-<config>-<platform>-<arch>` and copies the driver
library to `build/libadbc_driver_motherduck.{so,dylib,dll}`.

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
[Inherited tests](README.md#inherited-tests) for the details:

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

## License

By contributing, you agree that your contributions are licensed under the
[Apache License 2.0](LICENSE.txt).
