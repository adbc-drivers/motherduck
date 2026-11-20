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
///   doesn't use proxies or follow redirects.
///
/// \param ca_bundle A PEM file of CAs to trust in addition to the operating system's.
std::unique_ptr<HttpClient> MakeHttpClient(std::string ca_bundle);

}  // namespace adbcpq::auth
