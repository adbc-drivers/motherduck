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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "auth/login.h"
#include "driver/framework/status.h"

/// \file token_cache.h
/// Where browser login's MotherDuck token comes from: memory, then the credentials
/// file, then a login. The file holds {"version": 1, "motherduck_token": "<token>"}.

namespace adbcpq::auth {
using adbc::driver::Status;

class CredentialsLock;

struct Credentials {
  /// \brief The MotherDuck token, the password for the Postgres endpoint.
  std::string token;
  /// \brief The organization's Postgres endpoint host, if requested.
  std::string host;
  /// \brief Whether the token comes from a login just now, so that logging in
  ///   again if the endpoint rejects it would be pointless.
  bool fresh = false;
};

/// \brief MotherDuck tokens from browser login, cached in memory and in the
///   credentials file.
///
/// Thread-safe. Logins are serialized across the process, and across processes
/// sharing the credentials file, so concurrent connections wait for one login
/// rather than each starting their own. Within the process, a connection waits
/// for a login in progress however long it takes (the login ends by its own
/// timeout), and shares its outcome: its token, or its failure, so that a
/// login that times out or is denied doesn't open another browser.
class TokenCache {
 public:
  TokenCache(LoginEnvironment env, LoginOptions options)
      : env_(std::move(env)), options_(std::move(options)) {}

  /// \brief The number of logins the process has finished, to pass to Get as
  ///   since when a connection attempt begins.
  static uint64_t LoginsFinished();

  /// \brief Credentials to connect with, logging in through the browser unless a
  ///   saved token is usable.
  ///
  /// \param need_host Also look up the organization's Postgres endpoint.
  /// \param since LoginsFinished() when the connection attempt began. If a login
  ///   that finished since then (one in progress then, or started later)
  ///   failed, Get returns its failure instead of logging in again.
  Status Get(bool need_host, Credentials* credentials, uint64_t since);

  /// \brief Get, for a connection attempt that begins now.
  Status Get(bool need_host, Credentials* credentials) {
    return Get(need_host, credentials, LoginsFinished());
  }

  /// \brief Handle a token the Postgres endpoint rejected.
  ///
  /// Forgets the token, so the next Get logs in again, if the MotherDuck API
  /// rejects it too. The endpoint's error only hints at the cause, while the API
  /// answers for the token itself.
  ///
  /// \return Whether getting credentials again may help: the token was forgotten,
  ///   or another connection already replaced it.
  bool Reject(const std::string& token);

 private:
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;

  // When to stop waiting for another process's login: after the login timeout,
  // or never without one.
  Deadline LockDeadline() const;
  Status LockCredentials(const std::filesystem::path& path, const Deadline& deadline,
                         std::unique_ptr<CredentialsLock>* lock) const;
  Status Login(const std::filesystem::path& path, const std::string& rejected,
               const Deadline& deadline, uint64_t since, bool* fresh);
  void ForgetLocked(const std::filesystem::path& path, const Deadline& deadline);

  const LoginEnvironment env_;
  const LoginOptions options_;
  std::string token_;
  std::string host_;
  // After a denied login: when to open the browser again, and the failure.
  std::optional<std::chrono::steady_clock::time_point> denied_until_;
  std::string denied_message_;
};

/// \brief The lock that serializes logins across processes sharing a credentials
///   file. Released when destroyed, or by the OS if the process ends.
class CredentialsLock {
 public:
  /// \brief Take the lock of the credentials file at path.
  ///
  /// \param block Wait while another process holds it. Otherwise a lock held
  ///   elsewhere leaves *lock empty and returns OK.
  static Status Acquire(const std::filesystem::path& path,
                        std::unique_ptr<CredentialsLock>* lock, bool block = true);

  CredentialsLock(const CredentialsLock&) = delete;
  CredentialsLock& operator=(const CredentialsLock&) = delete;
  ~CredentialsLock();

 private:
  explicit CredentialsLock(intptr_t handle) : handle_(handle) {}

  // A file descriptor, or a Windows HANDLE.
  intptr_t handle_;
};

/// \brief A login in progress in some process, announced next to the credentials
///   file (in credentials.json.login) while that process holds its lock, so that
///   processes waiting for the lock know how long to wait.
struct LoginAnnouncement {
  int64_t pid = 0;
  /// \brief When the login ends at the latest.
  std::chrono::system_clock::time_point until;
};

/// \brief Announce that process pid is logging in through the credentials file
///   at path until at most until.
Status AnnounceLogin(const std::filesystem::path& path, int64_t pid,
                     std::chrono::system_clock::time_point until);

/// \brief The announced login, or nullopt if there's none or it's unreadable.
std::optional<LoginAnnouncement> ReadLoginAnnouncement(const std::filesystem::path& path);

void RemoveLoginAnnouncement(const std::filesystem::path& path);

/// \brief Whether a process with this ID is running.
bool ProcessIsRunning(int64_t pid);

/// \brief This process's ID.
int64_t CurrentProcessId();

/// \brief $MOTHERDUCK_HOME/credentials.json, or ~/.motherduck/credentials.json.
Status CredentialsPath(std::filesystem::path* path);

/// \brief The saved token, or nullopt if the file is missing or not a credentials
///   file.
std::optional<std::string> LoadSavedToken(const std::filesystem::path& path);

/// \brief Atomically replace the file, readable only by the user.
Status SaveToken(const std::filesystem::path& path, std::string_view token);

/// \brief Delete the file if it still holds this token.
void RemoveSavedToken(const std::filesystem::path& path, std::string_view token);

}  // namespace adbcpq::auth
