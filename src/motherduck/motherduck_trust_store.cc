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

#include "motherduck_trust_store.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <unistd.h>
#elif defined(_WIN32)
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#include <process.h>
#else
#include <unistd.h>
#endif

namespace adbcpq {

const std::vector<std::string> kLinuxCaBundlePaths = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Alpine, Arch
    "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL, CentOS, Fedora, Amazon Linux
    "/etc/ssl/ca-bundle.pem",              // openSUSE, SLES
    "/etc/ssl/cert.pem",                   // Alpine (alias)
};

namespace {
// The name the exported roots go under, in a per-user directory.
constexpr char kRootsFileName[] = "adbc_driver_motherduck_roots.pem";

#if defined(__APPLE__)
// Releases a Core Foundation object when it goes out of scope.
template <typename T>
class CFRef {
 public:
  CFRef() = default;
  explicit CFRef(T ref) : ref_(ref) {}
  CFRef(const CFRef&) = delete;
  CFRef& operator=(const CFRef&) = delete;
  ~CFRef() {
    if (ref_ != nullptr) CFRelease(ref_);
  }
  T get() const { return ref_; }
  T* out() { return &ref_; }

 private:
  T ref_ = nullptr;
};

// Whether a trust settings entry applies to TLS servers: it names no policy,
// or the SSL policy.
bool AppliesToTls(CFDictionaryRef entry) {
  const void* policy = CFDictionaryGetValue(entry, kSecTrustSettingsPolicy);
  if (policy == nullptr) return true;
  CFRef<CFDictionaryRef> properties(
      SecPolicyCopyProperties(static_cast<SecPolicyRef>(const_cast<void*>(policy))));
  if (properties.get() == nullptr) return false;
  const void* oid = CFDictionaryGetValue(properties.get(), kSecPolicyOid);
  return oid != nullptr && CFEqual(oid, kSecPolicyAppleSSL);
}

enum class Trust { kNoOpinion, kTrusted, kDenied };

// What a domain's trust settings say about a certificate as a TLS root, as
// SecTrustSettingsCopyTrustSettings documents: no entries means trusted as a
// root, and otherwise the first entry that applies and has a result decides.
Trust TlsTrust(SecCertificateRef cert, SecTrustSettingsDomain domain) {
  CFRef<CFArrayRef> settings;
  if (SecTrustSettingsCopyTrustSettings(cert, domain, settings.out()) != errSecSuccess ||
      settings.get() == nullptr) {
    return Trust::kNoOpinion;
  }
  CFIndex count = CFArrayGetCount(settings.get());
  if (count == 0) return Trust::kTrusted;
  for (CFIndex i = 0; i < count; i++) {
    auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(settings.get(), i));
    if (entry == nullptr || !AppliesToTls(entry)) continue;
    // A missing result means kSecTrustSettingsResultTrustRoot.
    SInt32 result = kSecTrustSettingsResultTrustRoot;
    auto number =
        static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kSecTrustSettingsResult));
    if (number != nullptr) CFNumberGetValue(number, kCFNumberSInt32Type, &result);
    switch (result) {
      case kSecTrustSettingsResultTrustRoot:
      case kSecTrustSettingsResultTrustAsRoot:
        return Trust::kTrusted;
      case kSecTrustSettingsResultDeny:
        return Trust::kDenied;
      default:
        continue;
    }
  }
  return Trust::kNoOpinion;
}

std::string CacheDirectory() {
  // A per-user directory only its owner can write to.
  char buffer[1024];
  size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, buffer, sizeof(buffer));
  if (length == 0 || length > sizeof(buffer)) return "";
  return buffer;
}
#elif defined(_WIN32)
// Whether a certificate's extended key usage allows TLS servers.
bool UsableForServerAuth(PCCERT_CONTEXT cert) {
  DWORD size = 0;
  if (!CertGetEnhancedKeyUsage(cert, 0, nullptr, &size)) {
    return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
  }
  std::vector<BYTE> buffer(size);
  auto usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
  if (!CertGetEnhancedKeyUsage(cert, 0, usage, &size)) {
    return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
  }
  // No usages means every usage, unless the properties rule them all out.
  if (usage->cUsageIdentifier == 0) {
    return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
  }
  for (DWORD i = 0; i < usage->cUsageIdentifier; i++) {
    if (std::string_view(usage->rgpszUsageIdentifier[i]) == szOID_PKIX_KP_SERVER_AUTH) {
      return true;
    }
  }
  return false;
}

std::string CacheDirectory() {
  // The user's temporary directory, in the ANSI code page that libpq opens
  // sslrootcert with.
  char buffer[MAX_PATH + 1];
  DWORD length = GetTempPathA(sizeof(buffer), buffer);
  if (length == 0 || length > sizeof(buffer)) return "";
  return std::string(buffer, length);
}
#endif

// Writes the native roots to a file in a per-user directory, returning its
// path, or empty if there are no roots or writing failed.
std::string ExportNativeRoots() {
#if defined(__APPLE__) || defined(_WIN32)
  std::string pem = NativeRootsPem();
  if (pem.empty()) return "";
  std::string directory = CacheDirectory();
  if (directory.empty()) return "";
  std::string path = (std::filesystem::path(directory) / kRootsFileName).string();
  // If another process holds the file open, Windows can't replace it.
  // WriteFileAtomically still succeeds when the file already holds these
  // roots; anything else at that path is not trusted, since the directory may
  // be shared (a service's temporary directory) and the name is predictable.
  if (!WriteFileAtomically(path, pem)) return "";
  return path;
#else
  return "";
#endif
}
}  // namespace

std::string FindCaBundle(const EnvLookup& get_env, const FileProbe& is_readable_file,
                         const std::vector<std::string>& bundle_paths,
                         const RootsExport& export_roots) {
  std::string env_path = get_env("SSL_CERT_FILE");
  if (!env_path.empty() && is_readable_file(env_path)) return env_path;
  for (const std::string& path : bundle_paths) {
    if (is_readable_file(path)) return path;
  }
  return export_roots();
}

std::string FindCaBundle(const EnvLookup& get_env) {
#if defined(__APPLE__) || defined(_WIN32)
  static const std::vector<std::string> kBundlePaths;
#else
  const std::vector<std::string>& kBundlePaths = kLinuxCaBundlePaths;
#endif
  return FindCaBundle(get_env, IsReadableFile, kBundlePaths, [] {
    // Exporting is slow, and the roots rarely change, so do it once.
    static const std::string path = ExportNativeRoots();
    return path;
  });
}

std::string DefaultRootCertPath(const EnvLookup& get_env) {
#ifdef _WIN32
  std::string appdata = get_env("APPDATA");
  if (appdata.empty()) return "";
  return appdata + "\\postgresql\\root.crt";
#else
  std::string home = get_env("HOME");
  if (home.empty()) return "";
  return home + "/.postgresql/root.crt";
#endif
}

bool IsReadableFile(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return false;
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

std::string PemCertificate(std::string_view der) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string base64;
  for (size_t i = 0; i < der.size(); i += 3) {
    uint32_t chunk = static_cast<unsigned char>(der[i]) << 16;
    if (i + 1 < der.size()) chunk |= static_cast<unsigned char>(der[i + 1]) << 8;
    if (i + 2 < der.size()) chunk |= static_cast<unsigned char>(der[i + 2]);
    base64 += kAlphabet[(chunk >> 18) & 0x3f];
    base64 += kAlphabet[(chunk >> 12) & 0x3f];
    base64 += i + 1 < der.size() ? kAlphabet[(chunk >> 6) & 0x3f] : '=';
    base64 += i + 2 < der.size() ? kAlphabet[chunk & 0x3f] : '=';
  }
  std::string pem = "-----BEGIN CERTIFICATE-----\n";
  for (size_t i = 0; i < base64.size(); i += 64) {
    pem += base64.substr(i, 64);
    pem += '\n';
  }
  pem += "-----END CERTIFICATE-----\n";
  return pem;
}

std::string NativeRootsPem() {
  std::string pem;
#if defined(__APPLE__)
  // The user's settings override the admin's, which override the system's,
  // so the first domain with an opinion on a certificate decides.
  std::map<std::string, Trust> certs;
  for (SecTrustSettingsDomain domain :
       {kSecTrustSettingsDomainUser, kSecTrustSettingsDomainAdmin,
        kSecTrustSettingsDomainSystem}) {
    CFRef<CFArrayRef> domain_certs;
    if (SecTrustSettingsCopyCertificates(domain, domain_certs.out()) != errSecSuccess ||
        domain_certs.get() == nullptr) {
      continue;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(domain_certs.get()); i++) {
      auto cert = static_cast<SecCertificateRef>(
          const_cast<void*>(CFArrayGetValueAtIndex(domain_certs.get(), i)));
      CFRef<CFDataRef> data(SecCertificateCopyData(cert));
      if (data.get() == nullptr) continue;
      std::string der(reinterpret_cast<const char*>(CFDataGetBytePtr(data.get())),
                      static_cast<size_t>(CFDataGetLength(data.get())));
      if (certs.count(der) != 0) continue;
      Trust trust = TlsTrust(cert, domain);
      if (trust != Trust::kNoOpinion) certs.emplace(std::move(der), trust);
    }
  }
  for (const auto& [der, trust] : certs) {
    if (trust == Trust::kTrusted) pem += PemCertificate(der);
  }
#elif defined(_WIN32)
  // The current user's ROOT store, which includes the machine's.
  HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
  if (store == nullptr) return "";
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
    if (!UsableForServerAuth(cert)) continue;
    pem += PemCertificate(std::string_view(
        reinterpret_cast<const char*>(cert->pbCertEncoded), cert->cbCertEncoded));
  }
  CertCloseStore(store, 0);
#endif
  return pem;
}

bool WriteFileAtomically(const std::string& path, std::string_view contents) {
  {
    std::ifstream existing(path, std::ios::binary);
    if (existing.good()) {
      std::string current((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
      if (current == contents) return true;
    }
  }
#ifdef _WIN32
  std::string temp_path = path + "." + std::to_string(_getpid()) + ".tmp";
#else
  std::string temp_path = path + "." + std::to_string(getpid()) + ".tmp";
#endif
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
      std::remove(temp_path.c_str());
      return false;
    }
  }
#ifdef _WIN32
  bool renamed =
      MoveFileExA(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  bool renamed = std::rename(temp_path.c_str(), path.c_str()) == 0;
#endif
  if (!renamed) std::remove(temp_path.c_str());
  return renamed;
}

}  // namespace adbcpq
