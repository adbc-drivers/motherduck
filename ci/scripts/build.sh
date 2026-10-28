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

set -euo pipefail

if [[ $# -ne 3 ]]; then
  printf 'usage: %s <test|release> <linux|macos|windows> <amd64|arm64>\n' "$0" >&2
  exit 2
fi

mode="$1"
platform="$2"
arch="$3"
case "$mode" in
  test) cmake_config=Debug; build_testing=ON ;;
  release) cmake_config=Release; build_testing=OFF ;;
  *) printf 'unsupported build mode: %s\n' "$mode" >&2; exit 2 ;;
esac
case "$platform" in
  linux) library_ext=so; triplet_platform=linux ;;
  macos) library_ext=dylib; triplet_platform=osx ;;
  windows) library_ext=dll; triplet_platform=windows-static ;;
  *) printf 'unsupported platform: %s\n' "$platform" >&2; exit 2 ;;
esac
case "$arch" in
  amd64) triplet_arch=x64 ;;
  arm64) triplet_arch=arm64 ;;
  *) printf 'unsupported architecture: %s\n' "$arch" >&2; exit 2 ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${repo_root}/build/ci-${mode}-${platform}-${arch}"

postgresql_tests=OFF
if [[ "$mode" == test && "$platform" == linux ]]; then
  postgresql_tests=ON
fi

configure_args=(
  -S "$repo_root" -B "$build_dir"
  -DCMAKE_BUILD_TYPE="$cmake_config"
  -DBUILD_TESTING="$build_testing"
  -DMOTHERDUCK_TEST_POSTGRESQL="${MOTHERDUCK_TEST_POSTGRESQL:-$postgresql_tests}"
)
if [[ "$platform" != windows ]] && command -v ninja >/dev/null 2>&1; then
  configure_args+=(-G Ninja)
fi
if [[ "$platform" == macos ]]; then
  if [[ "$arch" == amd64 ]]; then
    configure_args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64)
  else
    configure_args+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
  fi
fi

normalize_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s\n' "$1"
  fi
}

vcpkg_toolchain="${CMAKE_TOOLCHAIN_FILE:-}"
if [[ -z "$vcpkg_toolchain" ]]; then
  vcpkg_root="${VCPKG_ROOT:-${VCPKG_INSTALLATION_ROOT:-}}"
  if [[ -n "$vcpkg_root" ]]; then
    vcpkg_toolchain="$(normalize_path "$vcpkg_root")/scripts/buildsystems/vcpkg.cmake"
  fi
else
  vcpkg_toolchain="$(normalize_path "$vcpkg_toolchain")"
fi
if [[ -n "$vcpkg_toolchain" ]]; then
  if [[ ! -f "$vcpkg_toolchain" ]]; then
    printf 'vcpkg toolchain not found: %s\n' "$vcpkg_toolchain" >&2
    exit 1
  fi
  configure_args+=(
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg_toolchain"
    -DVCPKG_TARGET_TRIPLET="${triplet_arch}-${triplet_platform}"
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  )
fi

build_args=(--build "$build_dir" --config "$cmake_config" --parallel)
case "${CMAKE_VERBOSE:-}" in
  1 | ON | TRUE | true | yes | YES)
    configure_args+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
    build_args+=(--verbose)
    ;;
esac

cmake "${configure_args[@]}"
cmake "${build_args[@]}"

built_library="$(find "$build_dir" -type f \
  \( -name "libadbc_driver_motherduck.${library_ext}" -o -name "adbc_driver_motherduck.${library_ext}" \) \
  -print -quit)"
if [[ -z "$built_library" ]]; then
  printf 'built MotherDuck driver not found in %s\n' "$build_dir" >&2
  exit 1
fi
output_library="${repo_root}/build/libadbc_driver_motherduck.${library_ext}"
cp "$built_library" "$output_library"
chmod 755 "$output_library"
printf 'Built %s\n' "$output_library"
