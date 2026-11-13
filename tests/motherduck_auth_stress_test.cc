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

// Many threads connecting at once with browser login, as dbt does with one
// connection per thread: they should share one login, and its outcome, without
// data races. These use real time and threads, unlike motherduck_auth_test.cc.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "auth/http_client.h"
#include "auth/login.h"
#include "auth/token_cache.h"
#include "motherduck_test_util.h"

namespace {

namespace fs = std::filesystem;
using adbc::driver::Status;
using adbcpq::auth::Credentials;
using adbcpq::auth::FormFields;
using adbcpq::auth::HttpResponse;
using adbcpq::auth::LoginEnvironment;
using adbcpq::auth::LoginOptions;
using adbcpq::auth::TokenCache;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr int kThreads = 100;

/// A thread-safe fake of Auth0 and the MotherDuck API. The user approves a login
/// approve_after its device code is issued, and each login issues a new token.
class FakeServer : public adbcpq::auth::HttpClient {
 public:
  std::chrono::milliseconds approve_after{200};
  std::chrono::milliseconds exchange_delay{0};

  Status Get(const std::string& url, const std::string& token,
             HttpResponse* response) override {
    if (url.find("/mom/lookup-user-with-token") != std::string::npos) {
      std::lock_guard<std::mutex> lock(mutex_);
      *response = valid_.count(token) ? HttpResponse{200, R"({"pgEndpoint":"pg.test"})"}
                                      : HttpResponse{401, R"({"errMessage":"Invalid"})"};
      return Status::Ok();
    }
    // lookup-user: exchange the ID token for a MotherDuck token.
    std::this_thread::sleep_for(exchange_delay);
    std::lock_guard<std::mutex> lock(mutex_);
    exchanges_++;
    const std::string issued = "md-token-" + std::to_string(exchanges_);
    valid_[issued] = true;
    *response = {200,
                 R"({"shortLivedToken":")" + issued + R"(","pgEndpoint":"pg.test"})"};
    return Status::Ok();
  }

  Status PostForm(const std::string& url, const FormFields&,
                  HttpResponse* response) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (url.find("/oauth/device/code") != std::string::npos) {
      device_codes_++;
      approved_at_ = Clock::now() + approve_after;
      *response = {200, R"({"device_code":"d","user_code":"U",)"
                        R"("verification_uri_complete":"https://auth.test/u",)"
                        R"("expires_in":900,"interval":1})"};
    } else if (Clock::now() < approved_at_) {
      *response = {400, R"({"error":"authorization_pending"})"};
    } else {
      *response = {200, R"({"id_token":"id-token"})"};
    }
    return Status::Ok();
  }

  /// Whether the Postgres endpoint accepts the token.
  bool Accepts(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_.count(token) > 0;
  }
  void Allow(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    valid_[token] = true;
  }
  void Revoke(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    valid_.erase(token);
  }
  int device_codes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_codes_;
  }
  int exchanges() {
    std::lock_guard<std::mutex> lock(mutex_);
    return exchanges_;
  }

 private:
  std::mutex mutex_;
  std::map<std::string, bool> valid_;
  int device_codes_ = 0;
  int exchanges_ = 0;
  Clock::time_point approved_at_{};
};

class AuthStressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device random;
    home_ = fs::temp_directory_path() /
            ("motherduck_auth_stress_test_" + std::to_string(random()));
    fs::create_directories(home_);
    env_ = std::make_unique<ScopedEnv>("MOTHERDUCK_HOME", home_.string().c_str());
  }
  void TearDown() override {
    env_.reset();
    std::error_code ec;
    fs::remove_all(home_, ec);
  }

  std::shared_ptr<TokenCache> MakeCache(std::chrono::seconds timeout) {
    LoginEnvironment env;
    env.http = server_;
    env.print = [](const std::string&) {};
    env.open_browser = [this](const std::string&) {
      browsers_++;
      return true;
    };
    env.now = [] { return Clock::now(); };
    env.sleep = [](std::chrono::milliseconds duration) {
      std::this_thread::sleep_for(duration);
    };
    LoginOptions options;
    options.client_id = "client";
    options.auth_origin = "https://auth.test";
    options.api_origin = "https://api.test";
    options.timeout = timeout;
    return std::make_shared<TokenCache>(std::move(env), std::move(options));
  }

  /// What MotherduckDatabase::Connect does: get credentials, connect, and if the
  /// endpoint rejects a saved token, reject it and try once more.
  Status Connect(TokenCache& cache, bool need_host, std::string* token) {
    const uint64_t logins = TokenCache::LoginsFinished();
    Credentials credentials;
    UNWRAP_STATUS(cache.Get(need_host, &credentials, logins));
    if (!server_->Accepts(credentials.token)) {
      if (credentials.fresh || !cache.Reject(credentials.token)) {
        return Status(ADBC_STATUS_IO, "endpoint rejected the token");
      }
      UNWRAP_STATUS(cache.Get(need_host, &credentials, logins));
      if (!server_->Accepts(credentials.token)) {
        return Status(ADBC_STATUS_IO, "endpoint rejected the new token");
      }
    }
    *token = credentials.token + "@" + credentials.host;
    return Status::Ok();
  }

  struct Outcome {
    std::map<AdbcStatusCode, int> codes;
    std::map<std::string, int> tokens;
  };

  /// Connect from kThreads threads at once, thread i through caches[i % size].
  Outcome ConnectAll(const std::vector<std::shared_ptr<TokenCache>>& caches,
                     bool need_host) {
    Outcome outcome;
    std::mutex mutex;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
      threads.emplace_back([&, i] {
        while (!go) std::this_thread::yield();
        std::string token;
        const Status status = Connect(*caches[i % caches.size()], need_host, &token);
        std::lock_guard<std::mutex> lock(mutex);
        outcome.codes[status.ToAdbc(nullptr)]++;
        if (status.ok()) outcome.tokens[token]++;
      });
    }
    go = true;
    for (std::thread& thread : threads) thread.join();
    return outcome;
  }

  fs::path home_;
  std::unique_ptr<ScopedEnv> env_;
  std::shared_ptr<FakeServer> server_ = std::make_shared<FakeServer>();
  std::atomic<int> browsers_{0};
};

using Codes = std::map<AdbcStatusCode, int>;
using Tokens = std::map<std::string, int>;

TEST_F(AuthStressTest, ColdStartSharesOneLogin) {
  const Outcome outcome = ConnectAll({MakeCache(120s)}, /*need_host=*/true);
  EXPECT_EQ(outcome.codes, (Codes{{ADBC_STATUS_OK, kThreads}}));
  EXPECT_EQ(outcome.tokens, (Tokens{{"md-token-1@pg.test", kThreads}}));
  EXPECT_EQ(browsers_, 1);
  EXPECT_EQ(server_->device_codes(), 1);
}

TEST_F(AuthStressTest, DatabasesShareOneLoginThroughTheFile) {
  // One database (TokenCache) per thread.
  std::vector<std::shared_ptr<TokenCache>> caches;
  for (int i = 0; i < kThreads; i++) caches.push_back(MakeCache(120s));
  const Outcome outcome = ConnectAll(caches, /*need_host=*/true);
  EXPECT_EQ(outcome.codes, (Codes{{ADBC_STATUS_OK, kThreads}}));
  EXPECT_EQ(outcome.tokens, (Tokens{{"md-token-1@pg.test", kThreads}}));
  EXPECT_EQ(browsers_, 1);
}

TEST_F(AuthStressTest, ExpiredTokenSharesOneLogin) {
  for (const bool need_host : {true, false}) {
    SCOPED_TRACE(need_host ? "host looked up" : "host in the URI");
    ASSERT_TRUE(adbcpq::auth::SaveToken(home_ / "credentials.json", "old-token").ok());
    server_->Allow("old-token");
    std::shared_ptr<TokenCache> cache = MakeCache(120s);
    Credentials warm;
    ASSERT_TRUE(cache->Get(need_host, &warm).ok());
    server_->Revoke("old-token");
    const int browsers = browsers_;

    const Outcome outcome = ConnectAll({cache}, need_host);
    EXPECT_EQ(outcome.codes, (Codes{{ADBC_STATUS_OK, kThreads}}));
    EXPECT_EQ(outcome.tokens.size(), 1);
    EXPECT_EQ(browsers_ - browsers, 1);
  }
}

TEST_F(AuthStressTest, WaitsForLoginThatOutlastsTheTimeout) {
  // Approved in time, but the exchange ends after login_timeout: the threads
  // waiting for the login get its token rather than timing out.
  server_->approve_after = 500ms;
  server_->exchange_delay = 1500ms;
  const Outcome outcome = ConnectAll({MakeCache(2s)}, /*need_host=*/true);
  EXPECT_EQ(outcome.codes, (Codes{{ADBC_STATUS_OK, kThreads}}));
  EXPECT_EQ(browsers_, 1);
}

TEST_F(AuthStressTest, SharesTimedOutLogin) {
  // Never approved: one browser, and every connection fails with its timeout
  // instead of the next one starting another login.
  server_->approve_after = 1h;
  const auto start = Clock::now();
  const Outcome outcome = ConnectAll({MakeCache(2s)}, /*need_host=*/true);
  const auto elapsed = Clock::now() - start;
  EXPECT_EQ(outcome.codes, (Codes{{ADBC_STATUS_TIMEOUT, kThreads}}));
  EXPECT_EQ(browsers_, 1);
  EXPECT_EQ(server_->device_codes(), 1);
  EXPECT_LT(elapsed, 3500ms);

  // A connection attempt that starts after the failure logs in again.
  server_->approve_after = 0ms;
  std::string token;
  EXPECT_TRUE(Connect(*MakeCache(2s), /*need_host=*/true, &token).ok());
  EXPECT_EQ(browsers_, 2);
}

}  // namespace
