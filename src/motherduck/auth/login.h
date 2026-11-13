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
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "auth/http_client.h"
#include "driver/framework/status.h"

namespace adbcpq::auth {
using adbc::driver::Status;

/// \brief The Auth0 application of MotherDuck's DuckDB extension, which is set up
///   for SSO and for MotherDuck's sign-up and organization prompts.
inline constexpr std::string_view kDefaultClientId = "gfGhV0YsVtE7J7CSYgTquHwHLE9uGBMm";

/// \brief MotherDuck's Auth0 tenant, shared by production and staging.
inline constexpr std::string_view kAuthOrigin = "https://auth.motherduck.com";

/// \brief The effects of logging in, replaceable in tests.
struct LoginEnvironment {
  std::shared_ptr<HttpClient> http;
  /// \brief Show the user a message.
  std::function<void(const std::string&)> print;
  /// \brief Try to open a URL in a browser.
  std::function<bool(const std::string&)> open_browser;
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
};

/// \brief cpp-httplib, stderr, the default browser, and real time.
///
/// \param ca_bundle A PEM file of trusted CAs, or empty for the default.
LoginEnvironment DefaultLoginEnvironment(std::string ca_bundle);

struct LoginOptions {
  /// \brief The Auth0 application to log in through, such as kDefaultClientId.
  std::string client_id;
  /// \brief The Auth0 origin, such as kAuthOrigin.
  std::string auth_origin;
  /// \brief The MotherDuck API origin, such as ApiOrigin().
  std::string api_origin;
  /// \brief How long to wait for the user; zero waits until the login code expires.
  std::chrono::seconds timeout{};
};

/// \brief The MotherDuck API origin: https://api.motherduck.com, or the host in
///   motherduck_host (or MOTHERDUCK_HOST).
std::string ApiOrigin();

struct LoginResult {
  /// \brief A short-lived MotherDuck token (24 hours), the password for the
  ///   Postgres endpoint.
  std::string token;
  /// \brief The Postgres endpoint host of the user's organization.
  std::string pg_endpoint;
};

/// \brief Log in through the browser with the OAuth device authorization grant
///   (RFC 8628), then exchange the ID token for a MotherDuck token.
///
/// Blocks until the user approves or denies the login, or it times out.
Status DeviceLogin(const LoginEnvironment& env, const LoginOptions& options,
                   LoginResult* result);

/// \brief Look up the Postgres endpoint host of a MotherDuck token's organization.
///
/// Returns UNAUTHENTICATED if MotherDuck rejects the token (401 or 403). Other
/// failures, including 404, return IO.
Status LookupPgEndpoint(const LoginEnvironment& env, const LoginOptions& options,
                        const std::string& token, std::string* pg_endpoint);

}  // namespace adbcpq::auth
