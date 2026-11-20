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

#include "auth/login.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
// shellapi.h needs windows.h first.
#include <shellapi.h>
#else
#include <cerrno>

#include <spawn.h>
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#elif !defined(_WIN32)
extern char** environ;
#endif

#include <fmt/format.h>

#include "auth/env.h"
#include "auth/json.h"

namespace adbcpq::auth {

namespace {

using std::chrono::seconds;
using namespace std::chrono_literals;

constexpr std::string_view kAudience = "https://dev--06syqlm.us.auth0.com/api/v2/";
constexpr std::string_view kDeviceCodeGrant =
    "urn:ietf:params:oauth:grant-type:device_code";
constexpr seconds kDefaultInterval{5};
constexpr seconds kMaxInterval{60};
constexpr seconds kDefaultLifetime{300};
constexpr seconds kMaxLifetime{3600};
// RFC 8628 section 3.5: slow_down adds five seconds to the polling interval.
constexpr seconds kSlowDownStep{5};
constexpr seconds kExchangeDeadline{60};
constexpr std::chrono::milliseconds kExchangeRetryDelay{500};

/// The reason an Auth0 (error_description) or MotherDuck (errMessage) request failed.
std::string Reason(const JsonObject& body, long status) {
  for (const char* key : {"error_description", "errMessage", "error"}) {
    if (auto value = body.GetString(key); value && !value->empty()) return *value;
  }
  return fmt::format("HTTP {}", status);
}

seconds Clamp(std::optional<int64_t> value, seconds fallback, seconds max) {
  if (!value || *value <= 0) return fallback;
  return seconds(std::min<int64_t>(*value, max.count()));
}

Status PollForIdToken(const LoginEnvironment& env, const LoginOptions& options,
                      const std::string& device_code, seconds interval,
                      std::chrono::steady_clock::time_point deadline,
                      std::string* id_token) {
  const FormFields fields = {{"grant_type", std::string(kDeviceCodeGrant)},
                             {"device_code", device_code},
                             {"client_id", options.client_id}};
  while (true) {
    const auto remaining = deadline - env.now();
    if (remaining <= 0s) break;
    env.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::min<std::chrono::steady_clock::duration>(interval, remaining)));
    if (env.now() >= deadline) break;

    HttpResponse response;
    // On a network error, keep polling until the code expires.
    if (!env.http->PostForm(options.auth_origin + "/oauth/token", fields, &response)
             .ok()) {
      continue;
    }
    const JsonObject body = JsonObject::Parse(response.body);
    if (response.status == 200) {
      std::optional<std::string> token = body.GetString("id_token");
      if (!token || token->empty()) {
        return Status(ADBC_STATUS_IO,
                      "[motherduck] MotherDuck login returned no ID token");
      }
      *id_token = std::move(*token);
      return Status::Ok();
    }
    const std::string error = body.GetString("error").value_or("");
    if (error == "authorization_pending") continue;
    if (error == "slow_down" || response.status == 429) {
      interval = std::min(interval + kSlowDownStep, kMaxInterval);
      continue;
    }
    if (error == "expired_token") break;
    if (response.status >= 500) continue;
    return Status(ADBC_STATUS_UNAUTHENTICATED, "[motherduck] MotherDuck login failed: " +
                                                   Reason(body, response.status));
  }
  return Status(ADBC_STATUS_TIMEOUT,
                "[motherduck] MotherDuck login timed out before it was approved in the "
                "browser");
}

// Exchange an Auth0 ID token for a MotherDuck token. Retries for a while, since
// the account may still be getting set up.
Status ExchangeIdToken(const LoginEnvironment& env, const LoginOptions& options,
                       const std::string& id_token, LoginResult* result) {
  const auto deadline = env.now() + kExchangeDeadline;
  std::string last_error;
  do {
    HttpResponse response;
    Status status =
        env.http->Get(options.api_origin + "/mom/lookup-user", id_token, &response);
    if (status.ok()) {
      const JsonObject body = JsonObject::Parse(response.body);
      if (response.status == 200) {
        // Native clients, like the default one, get an access token without
        // expiry in `token` and a short-lived token in `shortLivedToken`; other
        // clients get only the short-lived one, in `token`.
        std::optional<std::string> token = body.GetString("shortLivedToken");
        if (!token || token->empty()) token = body.GetString("token");
        if (token && !token->empty()) {
          result->token = std::move(*token);
          result->pg_endpoint = body.GetString("pgEndpoint").value_or("");
          return Status::Ok();
        }
        last_error = "the response has no token";
      } else if (response.status == 401 || response.status == 403 ||
                 response.status == 404) {
        // Here 404 means there's no account for this login, or the selected
        // organization is gone, which retrying doesn't change.
        std::string reason = Reason(body, response.status);
        if (auto method = body.GetString("requiredAlternateLoginMethodDisplayName");
            method && !method->empty()) {
          reason += "; log in with " + *method;
        }
        return Status(ADBC_STATUS_UNAUTHENTICATED,
                      "[motherduck] MotherDuck login failed: " + reason);
      } else {
        last_error = Reason(body, response.status);
      }
    } else {
      last_error = status.message();
    }
    env.sleep(kExchangeRetryDelay);
  } while (env.now() < deadline);
  return Status(
      ADBC_STATUS_IO,
      "[motherduck] Could not exchange the login for a MotherDuck token: " + last_error);
}

bool IsHttps(const std::string& url) { return url.rfind("https://", 0) == 0; }

#if !defined(_WIN32)
char** Environment() {
#ifdef __APPLE__
  // Shared libraries can't refer to environ directly on macOS.
  return *_NSGetEnviron();
#else
  return environ;
#endif
}

bool HasDisplay() {
#if defined(__APPLE__)
  return true;
#else
  for (const char* name : {"DISPLAY", "WAYLAND_DISPLAY"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') return true;
  }
  return false;
#endif
}

#ifdef __APPLE__
constexpr const char* kOpenCommand = "open";
#else
constexpr const char* kOpenCommand = "xdg-open";
#endif
#endif

// Open an https URL in the default browser without waiting for it. Returns false
// if no browser was started, including on Linux and BSD without a display, where
// the fallback would be a console browser that takes over the terminal.
#ifdef _WIN32
bool OpenBrowser(const std::string& url) {
  if (!IsHttps(url)) return false;
  const int length = MultiByteToWideChar(CP_UTF8, 0, url.data(),
                                         static_cast<int>(url.size()), nullptr, 0);
  if (length <= 0) return false;
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), wide.data(),
                      length);
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  // ShellExecute returns a value greater than 32 on success.
  return reinterpret_cast<INT_PTR>(result) > 32;
}
#else
bool OpenBrowser(const std::string& url) {
  if (!IsHttps(url) || !HasDisplay()) return false;
  // sh starts the opener in the background and exits, so nothing waits for the
  // browser and no child is left to reap. The URL is an argument, never script text.
  const std::string script = std::string("command -v ") + kOpenCommand +
                             " >/dev/null 2>&1 || exit 1; " + kOpenCommand +
                             " \"$1\" </dev/null >/dev/null 2>&1 &";
  const char* argv[] = {"sh", "-c", script.c_str(), "sh", url.c_str(), nullptr};
  pid_t pid;
  if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, const_cast<char* const*>(argv),
                  Environment()) != 0) {
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    // ECHILD if the application ignores SIGCHLD; the opener may still have started.
    if (errno != EINTR) return errno == ECHILD;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

}  // namespace

LoginEnvironment DefaultLoginEnvironment(const std::string& extra_roots) {
  LoginEnvironment env;
  env.http = MakeHttpClient(extra_roots);
  env.print = [](const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
  };
  env.open_browser = OpenBrowser;
  env.now = [] { return std::chrono::steady_clock::now(); };
  env.sleep = [](std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
  };
  return env;
}

std::string ApiOrigin() {
  for (const char* name : {"motherduck_host", "MOTHERDUCK_HOST"}) {
    if (std::optional<std::string> host = GetEnvUtf8(name)) return "https://" + *host;
  }
  return "https://api.motherduck.com";
}

Status DeviceLogin(const LoginEnvironment& env, const LoginOptions& options,
                   LoginResult* result) {
  HttpResponse response;
  UNWRAP_STATUS(env.http->PostForm(options.auth_origin + "/oauth/device/code",
                                   {{"client_id", options.client_id},
                                    {"scope", "openid profile email"},
                                    {"audience", std::string(kAudience)}},
                                   &response));
  const JsonObject device = JsonObject::Parse(response.body);
  const std::optional<std::string> device_code = device.GetString("device_code");
  const std::optional<std::string> user_code = device.GetString("user_code");
  const std::optional<std::string> url = device.GetString("verification_uri_complete");
  if (response.status != 200 || !device_code || !user_code || !url) {
    return Status(ADBC_STATUS_IO, "[motherduck] MotherDuck login request failed: " +
                                      Reason(device, response.status));
  }

  env.print(fmt::format(
      "[motherduck] Log in to MotherDuck in your browser to continue.\n  URL:  {}\n  "
      "Code: {}",
      *url, *user_code));
  env.open_browser(*url);

  const seconds interval =
      Clamp(device.GetInt("interval"), kDefaultInterval, kMaxInterval);
  seconds lifetime = Clamp(device.GetInt("expires_in"), kDefaultLifetime, kMaxLifetime);
  if (options.timeout > 0s) lifetime = std::min(lifetime, options.timeout);
  std::string id_token;
  UNWRAP_STATUS(PollForIdToken(env, options, *device_code, interval, env.now() + lifetime,
                               &id_token));
  return ExchangeIdToken(env, options, id_token, result);
}

seconds MaxLoginDuration(const LoginOptions& options) {
  seconds wait = kMaxLifetime;
  if (options.timeout > 0s) wait = std::min(wait, options.timeout);
  // The device code request, the poll in flight at the deadline, and the
  // exchange's retries and its last request.
  return wait + kRequestTimeout + kRequestTimeout + kExchangeDeadline + kRequestTimeout;
}

Status LookupPgEndpoint(const LoginEnvironment& env, const LoginOptions& options,
                        const std::string& token, std::string* pg_endpoint) {
  HttpResponse response;
  UNWRAP_STATUS(env.http->Get(options.api_origin + "/mom/lookup-user-with-token", token,
                              &response));
  const JsonObject body = JsonObject::Parse(response.body);
  // Only an authentication failure rejects the token. A 404 (a deleted user, or
  // a wrong motherduck_host) isn't fixed by logging in again, which would also
  // delete the saved token.
  if (response.status == 401 || response.status == 403) {
    return Status(
        ADBC_STATUS_UNAUTHENTICATED,
        "[motherduck] MotherDuck rejected the token: " + Reason(body, response.status));
  }
  if (response.status != 200) {
    return Status(ADBC_STATUS_IO,
                  "[motherduck] Could not look up the MotherDuck Postgres endpoint: " +
                      Reason(body, response.status));
  }
  std::optional<std::string> endpoint = body.GetString("pgEndpoint");
  if (!endpoint || endpoint->empty()) {
    return Status(ADBC_STATUS_IO,
                  "[motherduck] MotherDuck didn't return a Postgres endpoint for the "
                  "token");
  }
  *pg_endpoint = std::move(*endpoint);
  return Status::Ok();
}

}  // namespace adbcpq::auth
