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

// Tests for finding the OS's trusted CA certificates.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "motherduck_trust_store.h"

namespace {
// Finds a bundle with the given environment and readable files, recording
// whether it exported the native roots.
class FindCaBundleTest : public ::testing::Test {
 protected:
  std::string Find(const std::vector<std::string>& bundle_paths) {
    exported_ = false;
    return adbcpq::FindCaBundle(
        [this](const char* name) {
          auto it = env_.find(name);
          return it == env_.end() ? std::string() : it->second;
        },
        [this](const std::string& path) { return readable_.count(path) != 0; },
        bundle_paths,
        [this] {
          exported_ = true;
          return native_;
        });
  }

  std::map<std::string, std::string> env_;
  std::set<std::string> readable_;
  std::string native_ = "/cache/roots.pem";
  bool exported_ = false;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}
}  // namespace

TEST_F(FindCaBundleTest, SslCertFileFirst) {
  env_["SSL_CERT_FILE"] = "/custom.pem";
  readable_ = {"/custom.pem", "/etc/ssl/certs/ca-certificates.crt"};
  EXPECT_EQ(Find(adbcpq::kLinuxCaBundlePaths), "/custom.pem");
  EXPECT_FALSE(exported_);
}

TEST_F(FindCaBundleTest, SkipsUnreadableSslCertFile) {
  env_["SSL_CERT_FILE"] = "/missing.pem";
  readable_ = {"/etc/pki/tls/certs/ca-bundle.crt"};
  EXPECT_EQ(Find(adbcpq::kLinuxCaBundlePaths), "/etc/pki/tls/certs/ca-bundle.crt");
}

TEST_F(FindCaBundleTest, FirstReadableBundlePath) {
  readable_ = {"/etc/ssl/cert.pem", "/etc/ssl/ca-bundle.pem"};
  EXPECT_EQ(Find(adbcpq::kLinuxCaBundlePaths), "/etc/ssl/ca-bundle.pem");
  EXPECT_FALSE(exported_);
  readable_ = {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"};
  EXPECT_EQ(Find(adbcpq::kLinuxCaBundlePaths), "/etc/ssl/certs/ca-certificates.crt");
}

TEST_F(FindCaBundleTest, NativeRootsLast) {
  // macOS and Windows have no bundle paths.
  readable_ = {"/etc/ssl/cert.pem"};
  EXPECT_EQ(Find({}), "/cache/roots.pem");
  EXPECT_TRUE(exported_);
  // On Linux, nothing is exported.
  readable_.clear();
  native_.clear();
  EXPECT_EQ(Find(adbcpq::kLinuxCaBundlePaths), "");
  EXPECT_TRUE(exported_);
}

TEST(TrustStoreTest, DefaultRootCertPath) {
  std::map<std::string, std::string> env = {{"HOME", "/home/me"},
                                            {"APPDATA", "C:\\Users\\me\\AppData"}};
  auto get_env = [&env](const char* name) { return env[name]; };
#ifdef _WIN32
  EXPECT_EQ(adbcpq::DefaultRootCertPath(get_env),
            "C:\\Users\\me\\AppData\\postgresql\\root.crt");
  env.erase("APPDATA");
#else
  EXPECT_EQ(adbcpq::DefaultRootCertPath(get_env), "/home/me/.postgresql/root.crt");
  env.erase("HOME");
#endif
  EXPECT_EQ(adbcpq::DefaultRootCertPath(get_env), "");
}

TEST(TrustStoreTest, PemCertificate) {
  EXPECT_EQ(adbcpq::PemCertificate("abc"),
            "-----BEGIN CERTIFICATE-----\nYWJj\n-----END CERTIFICATE-----\n");
  EXPECT_EQ(adbcpq::PemCertificate("ab"),
            "-----BEGIN CERTIFICATE-----\nYWI=\n-----END CERTIFICATE-----\n");
  EXPECT_EQ(adbcpq::PemCertificate(std::string(1, '\xff')),
            "-----BEGIN CERTIFICATE-----\n/w==\n-----END CERTIFICATE-----\n");
  // Lines hold 64 characters: 48 bytes.
  std::string pem = adbcpq::PemCertificate(std::string(49, '\0'));
  EXPECT_EQ(pem, "-----BEGIN CERTIFICATE-----\n" + std::string(64, 'A') +
                     "\nAA==\n-----END CERTIFICATE-----\n");
}

TEST(TrustStoreTest, IsReadableFile) {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "motherduck_trust_store_test_readable";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "file.pem") << "x";
  EXPECT_TRUE(adbcpq::IsReadableFile((dir / "file.pem").string()));
  EXPECT_FALSE(adbcpq::IsReadableFile(dir.string()));
  EXPECT_FALSE(adbcpq::IsReadableFile((dir / "missing.pem").string()));
  std::filesystem::remove_all(dir);
}

TEST(TrustStoreTest, WriteFileAtomically) {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "motherduck_trust_store_test_write";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::filesystem::path path = dir / "roots.pem";
  ASSERT_TRUE(adbcpq::WriteFileAtomically(path.string(), "first"));
  EXPECT_EQ(ReadFile(path), "first");
  ASSERT_TRUE(adbcpq::WriteFileAtomically(path.string(), "second"));
  EXPECT_EQ(ReadFile(path), "second");
  ASSERT_TRUE(adbcpq::WriteFileAtomically(path.string(), "second"));
  // No temporary files are left behind.
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
                          std::filesystem::directory_iterator()),
            1);
  // A directory that doesn't exist fails.
  EXPECT_FALSE(
      adbcpq::WriteFileAtomically((dir / "missing" / "roots.pem").string(), "x"));
  std::filesystem::remove_all(dir);
}

TEST(TrustStoreTest, NativeRootsPem) {
  std::string pem = adbcpq::NativeRootsPem();
#if defined(__APPLE__) || defined(_WIN32)
  // Every OS store has dozens of roots.
  size_t count = 0;
  for (size_t at = pem.find("-----BEGIN CERTIFICATE-----"); at != std::string::npos;
       at = pem.find("-----BEGIN CERTIFICATE-----", at + 1)) {
    count++;
  }
  EXPECT_GT(count, 10);
#else
  EXPECT_EQ(pem, "");
#endif
}

TEST(TrustStoreTest, FindCaBundle) {
  // Every supported OS has trusted roots, and CI runners have them.
  auto get_env = [](const char* name) {
    const char* value = std::getenv(name);
    return std::string(value ? value : "");
  };
  std::string bundle = adbcpq::FindCaBundle(get_env);
  ASSERT_NE(bundle, "");
  EXPECT_TRUE(adbcpq::IsReadableFile(bundle));
  // Exported once and cached.
  EXPECT_EQ(adbcpq::FindCaBundle(get_env), bundle);
}
