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

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "auth/http_client.h"
#include "auth/json.h"
#include "auth/login.h"
#include "auth/token_cache.h"
#include "motherduck_test_util.h"

namespace {

namespace fs = std::filesystem;
using adbc::driver::Status;
using adbcpq::auth::Credentials;
using adbcpq::auth::FormFields;
using adbcpq::auth::HttpResponse;
using adbcpq::auth::JsonObject;
using adbcpq::auth::LoginEnvironment;
using adbcpq::auth::LoginOptions;
using adbcpq::auth::LoginResult;
using adbcpq::auth::TokenCache;
using namespace std::chrono_literals;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Pair;

AdbcStatusCode Code(const Status& status) { return status.ToAdbc(nullptr); }

/// A fresh directory, removed with its contents at the end of the scope.
class TempDir {
 public:
  TempDir() {
    std::random_device random;
    path_ =
        fs::temp_directory_path() / ("motherduck_auth_test_" + std::to_string(random()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& path, const std::string& contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << contents;
}

/// Serves canned responses per URL, in order; the last one repeats.
class FakeHttp : public adbcpq::auth::HttpClient {
 public:
  struct Request {
    std::string url;
    std::string bearer_token;
    FormFields fields;
  };

  void Respond(const std::string& url, long status, std::string body) {
    responses_[url].push_back(HttpResponse{status, std::move(body)});
  }
  void Fail(const std::string& url) { responses_[url].push_back(std::nullopt); }

  Status Get(const std::string& url, const std::string& bearer_token,
             HttpResponse* response) override {
    requests.push_back({url, bearer_token, {}});
    return Serve(url, response);
  }

  Status PostForm(const std::string& url, const FormFields& fields,
                  HttpResponse* response) override {
    requests.push_back({url, "", fields});
    return Serve(url, response);
  }

  size_t Count(const std::string& url) const {
    size_t count = 0;
    for (const Request& request : requests) count += request.url == url;
    return count;
  }

  std::vector<Request> requests;

 private:
  Status Serve(const std::string& url, HttpResponse* response) {
    auto it = responses_.find(url);
    if (it == responses_.end() || it->second.empty()) {
      ADD_FAILURE() << "Unexpected request to " << url;
      return Status(ADBC_STATUS_IO, "unexpected request");
    }
    std::optional<HttpResponse> next = it->second.front();
    if (it->second.size() > 1) it->second.pop_front();
    if (!next) return Status(ADBC_STATUS_IO, "connection refused");
    *response = *next;
    return Status::Ok();
  }

  std::map<std::string, std::deque<std::optional<HttpResponse>>> responses_;
};

constexpr char kDeviceCodeUrl[] = "https://auth.test/oauth/device/code";
constexpr char kTokenUrl[] = "https://auth.test/oauth/token";
constexpr char kLookupUserUrl[] = "https://api.test/mom/lookup-user";
constexpr char kLookupUserWithTokenUrl[] = "https://api.test/mom/lookup-user-with-token";
constexpr char kVerificationUrl[] = "https://auth.test/activate?user_code=ABCD-EFGH";

constexpr char kDeviceCode[] = R"({
  "device_code": "device-code",
  "user_code": "ABCD-EFGH",
  "verification_uri": "https://auth.test/activate",
  "verification_uri_complete": "https://auth.test/activate?user_code=ABCD-EFGH",
  "expires_in": 900,
  "interval": 5
})";
constexpr char kPending[] =
    R"({"error":"authorization_pending","error_description":"User has yet to authorize device code."})";
constexpr char kSlowDown[] =
    R"({"error":"slow_down","error_description":"You are polling faster than allowed."})";
constexpr char kIdToken[] =
    R"({"id_token":"id-token","access_token":"a","expires_in":36000})";
constexpr char kLookupUser[] = R"({
  "userMetadata": {"email": "duck@example.com"},
  "token": "access-token",
  "shortLivedToken": "md-token",
  "regionalApiEndpoint": "https://api.us-east-1-aws.motherduck.com",
  "pgEndpoint": "pg.us-east-1-aws.motherduck.com",
  "region": "us-east-1-aws"
})";
constexpr char kLookupUserWithToken[] = R"({
  "userMetadata": {"email": "duck@example.com"},
  "pgEndpoint": "pg.eu-central-1-aws.motherduck.com",
  "region": "eu-central-1-aws"
})";

class LoginTest : public ::testing::Test {
 protected:
  void SetUp() override {
    http_ = std::make_shared<FakeHttp>();
    env_.http = http_;
    env_.print = [this](const std::string& message) { printed_.push_back(message); };
    env_.open_browser = [this](const std::string& url) {
      opened_.push_back(url);
      return true;
    };
    env_.now = [this] { return now_; };
    env_.sleep = [this](std::chrono::milliseconds duration) {
      sleeps_.push_back(duration);
      now_ += duration;
    };
    options_.client_id = adbcpq::auth::kDefaultClientId;
    options_.auth_origin = "https://auth.test";
    options_.api_origin = "https://api.test";
  }

  void RespondWithLogin() {
    http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
    http_->Respond(kTokenUrl, 200, kIdToken);
    http_->Respond(kLookupUserUrl, 200, kLookupUser);
  }

  std::shared_ptr<FakeHttp> http_;
  LoginEnvironment env_;
  LoginOptions options_;
  std::chrono::steady_clock::time_point now_;
  std::vector<std::string> printed_;
  std::vector<std::string> opened_;
  std::vector<std::chrono::milliseconds> sleeps_;
};

TEST(MotherDuckJsonTest, ReadsMembers) {
  const JsonObject object =
      JsonObject::Parse(R"({"s":"text","i":42,"d":7.9,"n":null,"o":{"s":"x"}})");
  ASSERT_TRUE(object.valid());
  EXPECT_EQ(object.GetString("s"), "text");
  EXPECT_EQ(object.GetInt("i"), 42);
  EXPECT_EQ(object.GetInt("d"), 7);
  EXPECT_EQ(object.GetString("i"), std::nullopt);
  EXPECT_EQ(object.GetInt("s"), std::nullopt);
  EXPECT_EQ(object.GetString("n"), std::nullopt);
  EXPECT_EQ(object.GetString("missing"), std::nullopt);
}

TEST(MotherDuckJsonTest, RejectsNonObjects) {
  for (const char* text : {"", "[]", "\"s\"", "42", "{", "{\"a\":1} x", "{'a':1}",
                           "{\"a\":1,}", "{\"a\":NaN}", "<html>Bad Gateway</html>"}) {
    EXPECT_FALSE(JsonObject::Parse(text).valid()) << text;
  }
  EXPECT_TRUE(JsonObject::Parse("{\"a\":1}\n").valid());
}

TEST(MotherDuckJsonTest, ReadsLargeNumbers) {
  const JsonObject object = JsonObject::Parse(
      R"({"big":18446744073709551615,"negative":-5,"huge":1e300,"tiny":-1e300})");
  EXPECT_EQ(object.GetInt("big"), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(object.GetInt("negative"), -5);
  EXPECT_EQ(object.GetInt("huge"), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(object.GetInt("tiny"), std::numeric_limits<int64_t>::min());
}

TEST(MotherDuckJsonTest, CredentialsJson) {
  EXPECT_EQ(adbcpq::auth::CredentialsJson("a.b/c\"d"),
            "{\n  \"version\": 1,\n  \"motherduck_token\": \"a.b/c\\\"d\"\n}\n");
  EXPECT_EQ(adbcpq::auth::CredentialsJson("\xff"), std::nullopt);
}

TEST_F(LoginTest, DeviceLogin) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403, kPending);
  http_->Respond(kTokenUrl, 429, kSlowDown);
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(kLookupUserUrl, 200, kLookupUser);

  LoginResult result;
  ASSERT_TRUE(adbcpq::auth::DeviceLogin(env_, options_, &result).ok());
  EXPECT_EQ(result.token, "md-token");
  EXPECT_EQ(result.pg_endpoint, "pg.us-east-1-aws.motherduck.com");

  ASSERT_EQ(printed_.size(), 1);
  EXPECT_THAT(printed_[0], HasSubstr(kVerificationUrl));
  EXPECT_THAT(printed_[0], HasSubstr("ABCD-EFGH"));
  EXPECT_THAT(opened_, ElementsAre(kVerificationUrl));
  // slow_down adds five seconds to the interval.
  EXPECT_THAT(sleeps_, ElementsAre(5s, 5s, 10s));

  ASSERT_EQ(http_->requests.size(), 5);
  EXPECT_THAT(http_->requests[0].fields,
              ElementsAre(Pair("client_id", std::string(adbcpq::auth::kDefaultClientId)),
                          Pair("scope", "openid profile email"),
                          Pair("audience", "https://dev--06syqlm.us.auth0.com/api/v2/")));
  EXPECT_THAT(
      http_->requests[1].fields,
      ElementsAre(Pair("grant_type", "urn:ietf:params:oauth:grant-type:device_code"),
                  Pair("device_code", "device-code"),
                  Pair("client_id", std::string(adbcpq::auth::kDefaultClientId))));
  EXPECT_EQ(http_->requests[4].url, kLookupUserUrl);
  EXPECT_EQ(http_->requests[4].bearer_token, "id-token");
}

TEST_F(LoginTest, DeviceLoginWithoutShortLivedToken) {
  // Clients the API doesn't treat as native get the short-lived token in `token`.
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(
      kLookupUserUrl, 200,
      R"({"token":"web-token","pgEndpoint":"pg.us-east-1-aws.motherduck.com"})");
  LoginResult result;
  ASSERT_TRUE(adbcpq::auth::DeviceLogin(env_, options_, &result).ok());
  EXPECT_EQ(result.token, "web-token");
}

TEST_F(LoginTest, DeviceLoginUsesClientId) {
  RespondWithLogin();
  options_.client_id = "other-client";
  LoginResult result;
  ASSERT_TRUE(adbcpq::auth::DeviceLogin(env_, options_, &result).ok());
  EXPECT_THAT(http_->requests[0].fields[0], Pair("client_id", "other-client"));
  EXPECT_THAT(http_->requests[1].fields[2], Pair("client_id", "other-client"));
}

TEST_F(LoginTest, DeviceLoginDenied) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403,
                 R"({"error":"access_denied","error_description":"User cancelled"})");
  LoginResult result;
  const Status status = adbcpq::auth::DeviceLogin(env_, options_, &result);
  EXPECT_EQ(Code(status), ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_THAT(status.message(), HasSubstr("User cancelled"));
  EXPECT_EQ(http_->Count(kLookupUserUrl), 0);
}

TEST_F(LoginTest, DeviceLoginCodeExpired) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403, kPending);
  http_->Respond(kTokenUrl, 403, R"({"error":"expired_token"})");
  LoginResult result;
  EXPECT_EQ(Code(adbcpq::auth::DeviceLogin(env_, options_, &result)),
            ADBC_STATUS_TIMEOUT);
}

TEST_F(LoginTest, DeviceLoginTimesOutWhenCodeExpires) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403, kPending);
  const auto start = now_;
  LoginResult result;
  EXPECT_EQ(Code(adbcpq::auth::DeviceLogin(env_, options_, &result)),
            ADBC_STATUS_TIMEOUT);
  EXPECT_EQ(now_ - start, 900s);
}

TEST_F(LoginTest, DeviceLoginTimeoutOption) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403, kPending);
  options_.timeout = 12s;
  const auto start = now_;
  LoginResult result;
  EXPECT_EQ(Code(adbcpq::auth::DeviceLogin(env_, options_, &result)),
            ADBC_STATUS_TIMEOUT);
  EXPECT_EQ(now_ - start, 12s);
  EXPECT_EQ(http_->Count(kTokenUrl), 2);
}

TEST_F(LoginTest, DeviceLoginRetriesNetworkErrors) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Fail(kTokenUrl);
  http_->Respond(kTokenUrl, 503, "<html>Service Unavailable</html>");
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Fail(kLookupUserUrl);
  http_->Respond(kLookupUserUrl, 502, "Bad Gateway");
  http_->Respond(kLookupUserUrl, 200, kLookupUser);
  LoginResult result;
  ASSERT_TRUE(adbcpq::auth::DeviceLogin(env_, options_, &result).ok());
  EXPECT_EQ(result.token, "md-token");
}

TEST_F(LoginTest, DeviceCodeRequestFails) {
  http_->Respond(
      kDeviceCodeUrl, 401,
      R"({"error":"unauthorized_client","error_description":"Grant type not allowed"})");
  LoginResult result;
  const Status status = adbcpq::auth::DeviceLogin(env_, options_, &result);
  EXPECT_EQ(Code(status), ADBC_STATUS_IO);
  EXPECT_THAT(status.message(), HasSubstr("Grant type not allowed"));
  EXPECT_TRUE(opened_.empty());
}

TEST_F(LoginTest, LookupUserRejectsAccount) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(kLookupUserUrl, 404,
                 R"({"errMessage":"User not found","mdFriendlyStatusCode":"NOT_FOUND",)"
                 R"("requiredAlternateLoginMethodDisplayName":"SSO"})");
  LoginResult result;
  const Status status = adbcpq::auth::DeviceLogin(env_, options_, &result);
  EXPECT_EQ(Code(status), ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_THAT(status.message(), HasSubstr("User not found; log in with SSO"));
}

TEST_F(LoginTest, LookupUserGivesUp) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(kLookupUserUrl, 500, R"({"errMessage":"Internal"})");
  LoginResult result;
  const Status status = adbcpq::auth::DeviceLogin(env_, options_, &result);
  EXPECT_EQ(Code(status), ADBC_STATUS_IO);
  EXPECT_THAT(status.message(), HasSubstr("Internal"));
}

TEST_F(LoginTest, LookupPgEndpoint) {
  http_->Respond(kLookupUserWithTokenUrl, 200, kLookupUserWithToken);
  std::string endpoint;
  ASSERT_TRUE(adbcpq::auth::LookupPgEndpoint(env_, options_, "md-token", &endpoint).ok());
  EXPECT_EQ(endpoint, "pg.eu-central-1-aws.motherduck.com");
  EXPECT_EQ(http_->requests[0].bearer_token, "md-token");
}

TEST_F(LoginTest, LookupPgEndpointRejected) {
  http_->Respond(
      kLookupUserWithTokenUrl, 403,
      R"({"errMessage":"Invalid token","mdFriendlyStatusCode":"AUTHENTICATION_ERROR"})");
  std::string endpoint;
  const Status status =
      adbcpq::auth::LookupPgEndpoint(env_, options_, "md-token", &endpoint);
  EXPECT_EQ(Code(status), ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_THAT(status.message(), HasSubstr("Invalid token"));
}

TEST_F(LoginTest, MaxLoginDuration) {
  // The wait for approval, plus three requests and the exchange's 60s of retries.
  options_.timeout = 120s;
  EXPECT_EQ(adbcpq::auth::MaxLoginDuration(options_), 270s);
  // Without a timeout, until the login code expires, at most after an hour.
  options_.timeout = 0s;
  EXPECT_EQ(adbcpq::auth::MaxLoginDuration(options_), 3750s);
  options_.timeout = std::chrono::seconds(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(adbcpq::auth::MaxLoginDuration(options_), 3750s);
}

TEST_F(LoginTest, LookupPgEndpointNotFound) {
  http_->Respond(kLookupUserWithTokenUrl, 404,
                 R"({"errMessage":"Route not found: GET /mom/lookup-user-with-token",)"
                 R"("mdFriendlyStatusCode":"NOT_FOUND"})");
  std::string endpoint;
  const Status status =
      adbcpq::auth::LookupPgEndpoint(env_, options_, "md-token", &endpoint);
  EXPECT_EQ(Code(status), ADBC_STATUS_IO);
  EXPECT_THAT(status.message(), HasSubstr("Route not found"));
}

TEST_F(LoginTest, LookupPgEndpointFails) {
  http_->Respond(kLookupUserWithTokenUrl, 500, "oops");
  std::string endpoint;
  EXPECT_EQ(Code(adbcpq::auth::LookupPgEndpoint(env_, options_, "md-token", &endpoint)),
            ADBC_STATUS_IO);
  http_->Respond(kLookupUserWithTokenUrl, 200, R"({"region":"us-east-1-aws"})");
  EXPECT_EQ(Code(adbcpq::auth::LookupPgEndpoint(env_, options_, "md-token", &endpoint)),
            ADBC_STATUS_IO);
}

class CredentialsFileTest : public ::testing::Test {
 protected:
  TempDir dir_;
  ScopedEnv home_{"MOTHERDUCK_HOME", dir_.path().string().c_str()};
  fs::path path_ = dir_.path() / "credentials.json";
};

TEST_F(CredentialsFileTest, PathFromMotherDuckHome) {
  fs::path path;
  ASSERT_TRUE(adbcpq::auth::CredentialsPath(&path).ok());
  EXPECT_EQ(path, path_);
}

TEST_F(CredentialsFileTest, RelativeMotherDuckHome) {
  ScopedEnv relative("MOTHERDUCK_HOME", "relative/dir");
  fs::path path;
  const Status status = adbcpq::auth::CredentialsPath(&path);
  EXPECT_EQ(Code(status), ADBC_STATUS_INVALID_ARGUMENT);
  EXPECT_THAT(status.message(), HasSubstr("relative/dir"));
}

#ifndef _WIN32
TEST_F(CredentialsFileTest, PathInHomeDirectory) {
  ScopedEnv no_override("MOTHERDUCK_HOME", nullptr);
  ScopedEnv home("HOME", dir_.path().string().c_str());
  fs::path path;
  ASSERT_TRUE(adbcpq::auth::CredentialsPath(&path).ok());
  EXPECT_EQ(path, dir_.path() / ".motherduck" / "credentials.json");
}
#endif

TEST_F(CredentialsFileTest, SaveAndLoad) {
  const fs::path path = dir_.path() / "new" / "credentials.json";
  ASSERT_TRUE(adbcpq::auth::SaveToken(path, "token-1").ok());
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path), "token-1");
  ASSERT_TRUE(adbcpq::auth::SaveToken(path, "token-2").ok());
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path), "token-2");
  // Only the file is left: the temporary file was renamed over it.
  EXPECT_EQ(
      std::distance(fs::directory_iterator(path.parent_path()), fs::directory_iterator()),
      1);
#ifndef _WIN32
  EXPECT_EQ(fs::status(path).permissions(),
            fs::perms::owner_read | fs::perms::owner_write);
  EXPECT_EQ(fs::status(path.parent_path()).permissions(), fs::perms::owner_all);
#endif
}

#ifndef _WIN32
TEST_F(CredentialsFileTest, SaveReplacesPlantedSymlink) {
  // Someone who can write to the directory links the temporary file elsewhere.
  const fs::path target = dir_.path() / "target";
  WriteFile(target, "keep");
  fs::create_symlink(
      target, dir_.path() / (".credentials.json." + std::to_string(getpid()) + ".tmp"));
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "token").ok());
  std::ifstream file(target);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "keep");
  EXPECT_FALSE(fs::is_symlink(path_));
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "token");
}
#endif

TEST_F(CredentialsFileTest, TrimsSavedToken) {
  WriteFile(path_,
            "{\n  \"version\": 1,\n  \"motherduck_token\": \" saved-token\\n\"\n}\n");
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
}

TEST_F(CredentialsFileTest, IgnoresInvalidFiles) {
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), std::nullopt);
  for (const char* contents : {"", "not json", R"({"motherduck_token":"t"})",
                               R"({"version":2,"motherduck_token":"t"})",
                               R"({"version":1,"motherduck_token":5})",
                               R"({"version":1,"motherduck_token":"  "})"}) {
    WriteFile(path_, contents);
    EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), std::nullopt) << contents;
  }
}

TEST_F(CredentialsFileTest, LoginAnnouncement) {
  EXPECT_EQ(adbcpq::auth::ReadLoginAnnouncement(path_), std::nullopt);
  const auto until = std::chrono::system_clock::now() + 90s;
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, 1234, until).ok());
  std::optional<adbcpq::auth::LoginAnnouncement> read =
      adbcpq::auth::ReadLoginAnnouncement(path_);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->pid, 1234);
  EXPECT_EQ(std::chrono::floor<std::chrono::milliseconds>(read->until),
            std::chrono::floor<std::chrono::milliseconds>(until));
  adbcpq::auth::RemoveLoginAnnouncement(path_);
  EXPECT_EQ(adbcpq::auth::ReadLoginAnnouncement(path_), std::nullopt);

  fs::path announcement = path_;
  announcement += ".login";
  for (const char* contents : {"", "1234", "x 5", "0 5", "-3 5", "12 5 extra", "12 x"}) {
    WriteFile(announcement, contents);
    EXPECT_EQ(adbcpq::auth::ReadLoginAnnouncement(path_), std::nullopt) << contents;
  }
}

TEST(MotherDuckProcessTest, ProcessIsRunning) {
  EXPECT_TRUE(adbcpq::auth::ProcessIsRunning(adbcpq::auth::CurrentProcessId()));
  EXPECT_FALSE(adbcpq::auth::ProcessIsRunning(0));
  EXPECT_FALSE(adbcpq::auth::ProcessIsRunning(-1));
  EXPECT_FALSE(adbcpq::auth::ProcessIsRunning(0x7ffffff0));
}

TEST_F(CredentialsFileTest, RemoveOnlyMatchingToken) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "token-1").ok());
  adbcpq::auth::RemoveSavedToken(path_, "token-2");
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "token-1");
  adbcpq::auth::RemoveSavedToken(path_, "token-1");
  EXPECT_FALSE(fs::exists(path_));
}

class TokenCacheTest : public LoginTest {
 protected:
  TokenCache MakeCache() { return TokenCache(env_, options_); }

  TempDir dir_;
  ScopedEnv home_{"MOTHERDUCK_HOME", dir_.path().string().c_str()};
  fs::path path_ = dir_.path() / "credentials.json";
};

TEST_F(TokenCacheTest, UsesSavedToken) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "saved-token");
  EXPECT_FALSE(credentials.fresh);
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, LooksUpHostOfSavedToken) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  http_->Respond(kLookupUserWithTokenUrl, 200, kLookupUserWithToken);
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/true, &credentials).ok());
  EXPECT_EQ(credentials.token, "saved-token");
  EXPECT_EQ(credentials.host, "pg.eu-central-1-aws.motherduck.com");
  ASSERT_TRUE(cache.Get(/*need_host=*/true, &credentials).ok());
  EXPECT_EQ(http_->Count(kLookupUserWithTokenUrl), 1);
}

TEST_F(TokenCacheTest, LogsInAndSaves) {
  RespondWithLogin();
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/true, &credentials).ok());
  EXPECT_EQ(credentials.token, "md-token");
  EXPECT_EQ(credentials.host, "pg.us-east-1-aws.motherduck.com");
  EXPECT_TRUE(credentials.fresh);
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "md-token");
  // lookup-user already gave the host.
  EXPECT_EQ(http_->Count(kLookupUserWithTokenUrl), 0);

  ASSERT_TRUE(cache.Get(/*need_host=*/true, &credentials).ok());
  EXPECT_FALSE(credentials.fresh);
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 1);
}

TEST_F(TokenCacheTest, LogsInWhenApiRejectsSavedToken) {
  // What the API answers for an expired or revoked token.
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "expired-token").ok());
  http_->Respond(
      kLookupUserWithTokenUrl, 403,
      R"({"errMessage":"Invalid authentication token, please fetch a new one )"
      R"(at https://app.motherduck.com","mdFriendlyStatusCode":"AUTHENTICATION_ERROR"})");
  RespondWithLogin();
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/true, &credentials).ok());
  EXPECT_EQ(credentials.token, "md-token");
  EXPECT_EQ(credentials.host, "pg.us-east-1-aws.motherduck.com");
  EXPECT_TRUE(credentials.fresh);
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "md-token");
}

TEST_F(TokenCacheTest, UsesTokenFromLoginInAnotherProcess) {
  // Holding the lock and announcing a login stands in for another process that
  // is logging in.
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, adbcpq::auth::CurrentProcessId(),
                                          std::chrono::system_clock::now() + 1h)
                  .ok());
  TokenCache cache = MakeCache();
  Credentials credentials;
  std::future<Status> result = std::async(
      std::launch::async, [&] { return cache.Get(/*need_host=*/false, &credentials); });
  EXPECT_EQ(result.wait_for(200ms), std::future_status::timeout);

  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "other-token").ok());
  other.reset();
  ASSERT_TRUE(result.get().ok());
  EXPECT_EQ(credentials.token, "other-token");
  EXPECT_FALSE(credentials.fresh);
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, TimesOutWaitingForLoginInAnotherProcess) {
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  options_.timeout = 5s;
  TokenCache cache = MakeCache();
  Credentials credentials;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)), ADBC_STATUS_TIMEOUT);
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, WaitsPastTimeoutForAnnouncedLogin) {
  // Another process announced a login that may take an hour: wait for it past
  // this database's own login timeout, then use its token.
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, adbcpq::auth::CurrentProcessId(),
                                          std::chrono::system_clock::now() + 1h)
                  .ok());
  options_.timeout = 5s;
  const auto start = now_;
  env_.sleep = [&](std::chrono::milliseconds duration) {
    now_ += duration;
    if (other && now_ - start > 20s) {
      ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "other-token").ok());
      adbcpq::auth::RemoveLoginAnnouncement(path_);
      other.reset();
    }
  };
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "other-token");
  EXPECT_FALSE(credentials.fresh);
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, TimesOutWhenAnnouncedLoginIsOverdue) {
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, adbcpq::auth::CurrentProcessId(),
                                          std::chrono::system_clock::now() - 1h)
                  .ok());
  TokenCache cache = MakeCache();
  Credentials credentials;
  const Status status = cache.Get(/*need_host=*/false, &credentials);
  EXPECT_EQ(Code(status), ADBC_STATUS_TIMEOUT);
  EXPECT_THAT(status.message(), HasSubstr("longer than its timeout"));
  EXPECT_TRUE(sleeps_.empty());
}

TEST_F(TokenCacheTest, IgnoresAnnouncementOfEndedProcess) {
  // A crashed process's announcement is left behind; the lock holder is a
  // process that isn't logging in, so wait the login timeout.
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, /*pid=*/0x7ffffff0,
                                          std::chrono::system_clock::now() + 1h)
                  .ok());
  options_.timeout = 5s;
  TokenCache cache = MakeCache();
  Credentials credentials;
  const Status status = cache.Get(/*need_host=*/false, &credentials);
  EXPECT_EQ(Code(status), ADBC_STATUS_TIMEOUT);
  EXPECT_THAT(status.message(), HasSubstr("waiting for another MotherDuck login"));
}

TEST_F(TokenCacheTest, BoundsUnannouncedWaitWithoutTimeout) {
  // login_timeout=0 used to wait forever for a lock held without a login.
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  options_.timeout = 0s;
  TokenCache cache = MakeCache();
  Credentials credentials;
  const auto start = now_;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)), ADBC_STATUS_TIMEOUT);
  EXPECT_GE(now_ - start, 30s);
  EXPECT_LT(now_ - start, 31s);
}

TEST_F(TokenCacheTest, AnnouncesLoginWhileLoggingIn) {
  RespondWithLogin();
  std::optional<adbcpq::auth::LoginAnnouncement> during;
  env_.open_browser = [&](const std::string&) {
    during = adbcpq::auth::ReadLoginAnnouncement(path_);
    return true;
  };
  options_.timeout = 120s;
  TokenCache cache = MakeCache();
  Credentials credentials;
  const auto before = std::chrono::system_clock::now();
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  ASSERT_TRUE(during.has_value());
  EXPECT_EQ(during->pid, adbcpq::auth::CurrentProcessId());
  EXPECT_GE(during->until, std::chrono::floor<std::chrono::milliseconds>(
                               before + adbcpq::auth::MaxLoginDuration(options_)));
  EXPECT_LE(during->until,
            std::chrono::system_clock::now() + adbcpq::auth::MaxLoginDuration(options_));
  EXPECT_EQ(adbcpq::auth::ReadLoginAnnouncement(path_), std::nullopt);
}

TEST_F(TokenCacheTest, PausesAfterDeniedLogin) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403,
                 R"({"error":"access_denied","error_description":"User denied"})");
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(kLookupUserUrl, 200, kLookupUser);
  TokenCache cache = MakeCache();
  Credentials credentials;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)),
            ADBC_STATUS_UNAUTHENTICATED);

  // A new connection attempt within a minute fails without opening the
  // browser, in this database and in a new one for the same account, as dbt
  // may create.
  now_ += 30s;
  const Status status = cache.Get(/*need_host=*/false, &credentials);
  EXPECT_EQ(Code(status), ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_THAT(status.message(), HasSubstr("User denied"));
  EXPECT_THAT(status.message(),
              HasSubstr("not opening the browser again for 30 seconds"));
  TokenCache other = MakeCache();
  EXPECT_EQ(Code(other.Get(/*need_host=*/false, &credentials)),
            ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 1);
  EXPECT_EQ(opened_.size(), 1);

  // After it, log in again.
  now_ += 31s;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "md-token");
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 2);
}

TEST_F(TokenCacheTest, FailuresAreKeptPerAccount) {
  // A database with another Auth0 application neither shares the failure of a
  // login it overlapped nor pauses after it.
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 401,
                 R"({"error":"invalid_client","error_description":"Unknown client"})");
  http_->Respond(kTokenUrl, 200, kIdToken);
  http_->Respond(kLookupUserUrl, 200, kLookupUser);
  const uint64_t since = TokenCache::LoginsFinished();
  options_.client_id = "wrong-client";
  TokenCache wrong = MakeCache();
  options_.client_id = adbcpq::auth::kDefaultClientId;
  TokenCache right = MakeCache();
  Credentials credentials;
  const Status failed = wrong.Get(/*need_host=*/false, &credentials, since);
  EXPECT_EQ(Code(failed), ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_THAT(failed.message(), HasSubstr("Unknown client"));
  ASSERT_TRUE(right.Get(/*need_host=*/false, &credentials, since).ok());
  EXPECT_EQ(credentials.token, "md-token");
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 2);
}

TEST_F(TokenCacheTest, WaitsWhileAnnouncedLoginSavesItsToken) {
  // The other process's login ends after this database's login timeout; it
  // then keeps the lock a little longer to save its token.
  std::unique_ptr<adbcpq::auth::CredentialsLock> other;
  ASSERT_TRUE(adbcpq::auth::CredentialsLock::Acquire(path_, &other).ok());
  ASSERT_TRUE(adbcpq::auth::AnnounceLogin(path_, adbcpq::auth::CurrentProcessId(),
                                          std::chrono::system_clock::now() + 1h)
                  .ok());
  options_.timeout = 5s;
  const auto start = now_;
  env_.sleep = [&](std::chrono::milliseconds duration) {
    now_ += duration;
    if (now_ - start > 20s) adbcpq::auth::RemoveLoginAnnouncement(path_);
    if (other && now_ - start > 25s) {
      ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "other-token").ok());
      other.reset();
    }
  };
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "other-token");
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, KeepsAnnouncementUntilTokenIsSaved) {
  // Saving fails (a directory is in the way), so Login prints a warning right
  // after trying to save: the announcement must still be there then.
  RespondWithLogin();
  fs::create_directories(path_);
  std::optional<adbcpq::auth::LoginAnnouncement> while_saving;
  env_.print = [&](const std::string& message) {
    if (message.find("log in again next time") != std::string::npos) {
      while_saving = adbcpq::auth::ReadLoginAnnouncement(path_);
    }
  };
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_TRUE(while_saving.has_value());
  EXPECT_EQ(adbcpq::auth::ReadLoginAnnouncement(path_), std::nullopt);
}

TEST_F(TokenCacheTest, NoPauseAfterTimedOutLogin) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 400, kPending);
  options_.timeout = 10s;
  TokenCache cache = MakeCache();
  Credentials credentials;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)), ADBC_STATUS_TIMEOUT);
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)), ADBC_STATUS_TIMEOUT);
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 2);
}

TEST_F(TokenCacheTest, LoginFailure) {
  http_->Respond(kDeviceCodeUrl, 200, kDeviceCode);
  http_->Respond(kTokenUrl, 403, R"({"error":"access_denied"})");
  TokenCache cache = MakeCache();
  Credentials credentials;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/false, &credentials)),
            ADBC_STATUS_UNAUTHENTICATED);
  EXPECT_FALSE(fs::exists(path_));
}

TEST_F(TokenCacheTest, RejectLogsInAgain) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  http_->Respond(kLookupUserWithTokenUrl, 403, R"({"errMessage":"Invalid token"})");
  RespondWithLogin();
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_TRUE(cache.Reject("saved-token"));
  EXPECT_EQ(http_->requests[0].bearer_token, "saved-token");
  EXPECT_FALSE(fs::exists(path_));
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "md-token");
  EXPECT_TRUE(credentials.fresh);
}

TEST_F(TokenCacheTest, RejectIgnoresReplacedToken) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_TRUE(cache.Reject("older-token"));
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "saved-token");
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
  EXPECT_TRUE(http_->requests.empty());
}

TEST_F(TokenCacheTest, RejectKeepsTokenTheApiAccepts) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  http_->Respond(kLookupUserWithTokenUrl, 200, kLookupUserWithToken);
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_FALSE(cache.Reject("saved-token"));
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "saved-token");
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 0);
}

TEST_F(TokenCacheTest, KeepsTokenWhenHostLookupNotFound) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  http_->Respond(kLookupUserWithTokenUrl, 404,
                 R"({"errMessage":"User not found","mdFriendlyStatusCode":"NOT_FOUND"})");
  TokenCache cache = MakeCache();
  Credentials credentials;
  EXPECT_EQ(Code(cache.Get(/*need_host=*/true, &credentials)), ADBC_STATUS_IO);
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
  EXPECT_FALSE(cache.Reject("saved-token"));
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
  EXPECT_EQ(http_->Count(kDeviceCodeUrl), 0);
}

TEST_F(TokenCacheTest, RejectKeepsTokenIfApiUnreachable) {
  ASSERT_TRUE(adbcpq::auth::SaveToken(path_, "saved-token").ok());
  http_->Fail(kLookupUserWithTokenUrl);
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_FALSE(cache.Reject("saved-token"));
  EXPECT_EQ(adbcpq::auth::LoadSavedToken(path_), "saved-token");
}

TEST_F(TokenCacheTest, KeepsTokenIfSavingFails) {
  // A file where the credentials directory should be.
  WriteFile(dir_.path() / "blocked", "");
  ScopedEnv blocked("MOTHERDUCK_HOME", (dir_.path() / "blocked").string().c_str());
  RespondWithLogin();
  TokenCache cache = MakeCache();
  Credentials credentials;
  ASSERT_TRUE(cache.Get(/*need_host=*/false, &credentials).ok());
  EXPECT_EQ(credentials.token, "md-token");
  ASSERT_EQ(printed_.size(), 2);
  EXPECT_THAT(printed_[1], HasSubstr("log in again next time"));
}

}  // namespace
