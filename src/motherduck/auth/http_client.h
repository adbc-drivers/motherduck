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

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "driver/framework/status.h"

namespace adbcpq::auth {
using adbc::driver::Status;

struct HttpResponse {
  long status = 0;
  std::string body;
};

using FormFields = std::vector<std::pair<std::string, std::string>>;

/// \brief The longest one request of MakeHttpClient's client takes, from
///   connecting to the last byte of the response.
inline constexpr std::chrono::seconds kRequestTimeout{30};

/// \brief The HTTPS requests that login makes.
///
/// A non-OK status means no response arrived; an HTTP error status is a response.
class HttpClient {
 public:
  virtual ~HttpClient() = default;

  /// \brief GET a URL with a bearer token.
  virtual Status Get(const std::string& url, const std::string& bearer_token,
                     HttpResponse* response) = 0;

  /// \brief POST an application/x-www-form-urlencoded body.
  virtual Status PostForm(const std::string& url, const FormFields& fields,
                          HttpResponse* response) = 0;
};

/// \brief An HttpClient using cpp-httplib and OpenSSL, restricted to HTTPS. It
///   doesn't use proxies or follow redirects, and trusts the system's CAs.
///
/// \param extra_roots A PEM file of CAs to trust as well, or empty. One that
///   can't be read is skipped.
std::unique_ptr<HttpClient> MakeHttpClient(const std::string& extra_roots);

/// \brief The PEM certificates the client trusts beyond what OpenSSL finds on
///   its own: extra_roots, if readable, and on Linux the system's bundle when a
///   statically linked OpenSSL looks for it in the wrong place.
std::string ExtraRootsPem(const std::string& extra_roots);

}  // namespace adbcpq::auth
