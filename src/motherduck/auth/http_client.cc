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

// For #warning Please include winsock2.h before windows.h on RTools/msys2
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "auth/http_client.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include "adbc_driver_motherduck_config.h"

namespace adbcpq::auth {

namespace {

constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadWriteTimeoutSeconds = 30;
constexpr std::chrono::seconds kRequestTimeout{30};
constexpr size_t kMaxResponseBytes = 1 << 20;
constexpr std::string_view kHttps = "https://";

#if !defined(_WIN32) && !defined(__APPLE__)
// CA bundles of common Linux distributions, as Go's crypto/x509 lists them.
constexpr const char* kSystemCaBundles[] = {
    "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch
    "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL 6
    "/etc/ssl/ca-bundle.pem",                             // openSUSE
    "/etc/pki/tls/cacert.pem",                            // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS, RHEL 7
    "/etc/ssl/cert.pem",                                  // Alpine
};

// A statically linked OpenSSL looks for CAs where its build machine kept them. If
// they aren't here, and SSL_CERT_FILE or SSL_CERT_DIR doesn't say otherwise, use
// the system's bundle. (httplib uses the Windows store and the macOS keychain.)
std::string SystemCaBundle() {
  if (std::getenv(X509_get_default_cert_file_env()) ||
      std::getenv(X509_get_default_cert_dir_env())) {
    return "";
  }
  std::error_code ec;
  if (std::filesystem::is_regular_file(X509_get_default_cert_file(), ec) ||
      std::filesystem::is_directory(X509_get_default_cert_dir(), ec)) {
    return "";
  }
  for (const char* bundle : kSystemCaBundles) {
    if (std::filesystem::is_regular_file(bundle, ec)) return bundle;
  }
  return "";
}
#endif

// Split https://host[:port]/path into its origin and path.
bool SplitUrl(const std::string& url, std::string* origin, std::string* path) {
  if (url.compare(0, kHttps.size(), kHttps) != 0) return false;
  const size_t slash = url.find('/', kHttps.size());
  *origin = url.substr(0, slash);
  *path = slash == std::string::npos ? "/" : url.substr(slash);
  return origin->size() > kHttps.size();
}

std::string Describe(const httplib::Result& result) {
  std::string message = httplib::to_string(result.error());
  const uint64_t ssl_error = result.ssl_backend_error();
  if (ssl_error == 0) return message;
  message += " (";
  if (result.error() == httplib::Error::SSLServerVerification) {
    // For a rejected certificate, httplib reports X509_verify_cert's result.
    message += X509_verify_cert_error_string(static_cast<long>(ssl_error));
  } else {
    char buffer[256];
    ERR_error_string_n(static_cast<unsigned long>(ssl_error), buffer, sizeof(buffer));
    message += buffer;
  }
  message += ")";
  return message;
}

class HttplibClient : public HttpClient {
 public:
  explicit HttplibClient(std::string ca_bundle) : ca_bundle_(std::move(ca_bundle)) {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (ca_bundle_.empty()) ca_bundle_ = SystemCaBundle();
#endif
  }

  Status Get(const std::string& url, const std::string& bearer_token,
             HttpResponse* response) override {
    return Send(url, response, [&](httplib::Client& client, const std::string& path) {
      return client.Get(path,
                        httplib::Headers{{"Authorization", "Bearer " + bearer_token}});
    });
  }

  Status PostForm(const std::string& url, const FormFields& fields,
                  HttpResponse* response) override {
    const httplib::Params params(fields.begin(), fields.end());
    return Send(url, response, [&](httplib::Client& client, const std::string& path) {
      return client.Post(path, httplib::Headers{}, params);
    });
  }

 private:
  template <typename Request>
  Status Send(const std::string& url, HttpResponse* response, Request&& request) {
    std::string origin;
    std::string path;
    if (!SplitUrl(url, &origin, &path)) {
      return Status(ADBC_STATUS_INVALID_ARGUMENT,
                    "[motherduck] Not an https URL: " + url);
    }
    try {
      httplib::Client client(origin);
      client.set_connection_timeout(kConnectTimeoutSeconds);
      client.set_read_timeout(kReadWriteTimeoutSeconds);
      client.set_write_timeout(kReadWriteTimeoutSeconds);
      client.set_max_timeout(kRequestTimeout);
      client.set_follow_location(false);
      client.set_payload_max_length(kMaxResponseBytes);
      client.enable_server_certificate_verification(true);
      client.enable_server_hostname_verification(true);
      client.set_default_headers(
          {{"User-Agent", "adbc-driver-motherduck/" ADBC_DRIVER_MOTHERDUCK_VERSION}});
      if (!ca_bundle_.empty()) client.set_ca_cert_path(ca_bundle_);
      // Trust the system's CAs too: a bundle meant for the Postgres endpoint
      // shouldn't break the login's requests.
      client.enable_system_ca(true);

      httplib::Result result = request(client, path);
      if (!result) {
        return Status(ADBC_STATUS_IO,
                      "[motherduck] Request to " + url + " failed: " + Describe(result));
      }
      response->status = result->status;
      response->body = std::move(result->body);
      return Status::Ok();
    } catch (const std::exception& e) {
      return Status(ADBC_STATUS_IO,
                    "[motherduck] Request to " + url + " failed: " + e.what());
    }
  }

  std::string ca_bundle_;
};

}  // namespace

std::unique_ptr<HttpClient> MakeHttpClient(std::string ca_bundle) {
  return std::make_unique<HttplibClient>(std::move(ca_bundle));
}

}  // namespace adbcpq::auth
