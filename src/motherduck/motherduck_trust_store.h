// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Finding the OS's trusted CA certificates for libpq. The driver's statically
// linked libpq and OpenSSL take roots only from a file, and can't find the
// OS's own with sslrootcert=system.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace adbcpq {

/// \brief An environment variable's value, or empty if it's unset.
using EnvLookup = std::function<std::string(const char* name)>;
/// \brief Whether path names a regular file the process can read.
using FileProbe = std::function<bool(const std::string& path)>;
/// \brief Writes the OS's trusted roots to a PEM file, returning its path, or
/// empty if there are none.
using RootsExport = std::function<std::string()>;

/// \brief Where Linux distributions keep their CA bundle, in the order tried.
extern const std::vector<std::string> kLinuxCaBundlePaths;

/// \brief A PEM CA bundle for verifying servers, or empty if there's none.
///
/// Tries SSL_CERT_FILE, then the first readable file of bundle_paths, then
/// export_roots. Unreadable files are skipped.
std::string FindCaBundle(const EnvLookup& get_env, const FileProbe& is_readable_file,
                         const std::vector<std::string>& bundle_paths,
                         const RootsExport& export_roots);

/// \brief FindCaBundle for this process: kLinuxCaBundlePaths on Linux, and the
/// native trust store, exported once per process, on macOS and Windows.
std::string FindCaBundle(const EnvLookup& get_env);

/// \brief libpq's default sslrootcert, used when the connection doesn't set
/// one: ~/.postgresql/root.crt, or %APPDATA%\\postgresql\\root.crt on Windows.
std::string DefaultRootCertPath(const EnvLookup& get_env);

/// \brief Whether path names a regular file the process can read.
bool IsReadableFile(const std::string& path);

/// \brief A DER certificate as a PEM block.
std::string PemCertificate(std::string_view der);

/// \brief The native trust store's roots for TLS servers, as PEM: the
/// Security framework's trust settings on macOS, and the ROOT system store on
/// Windows. Always empty elsewhere.
std::string NativeRootsPem();

/// \brief Replaces path's contents, by writing a temporary file next to it and
/// renaming it, so readers never see a partial file. Leaves the file alone if
/// it already holds contents.
bool WriteFileAtomically(const std::string& path, std::string_view contents);

}  // namespace adbcpq
