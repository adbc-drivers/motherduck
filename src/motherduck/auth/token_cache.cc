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

#include "auth/token_cache.h"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <process.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "auth/json.h"

namespace adbcpq::auth {

namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxFileBytes = 64 * 1024;

std::string PathString(const fs::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

#ifdef _WIN32
std::optional<fs::path> EnvPath(const wchar_t* name) {
  wchar_t* value = nullptr;
  size_t length = 0;
  if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
  std::optional<fs::path> path;
  if (*value != L'\0') path = fs::path(value);
  std::free(value);
  return path;
}

std::optional<fs::path> MotherDuckHome() { return EnvPath(L"MOTHERDUCK_HOME"); }

std::optional<fs::path> HomeDirectory() { return EnvPath(L"USERPROFILE"); }

int ProcessId() { return _getpid(); }

// A security descriptor that lets only the current user access a file, whatever
// the directory passes on, or null. Free it with LocalFree.
PSECURITY_DESCRIPTOR UserOnlySecurityDescriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  const bool got =
      size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!got) return nullptr;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                              &sid)) {
    return nullptr;
  }
  // P: protected, so nothing is inherited. FA: full access.
  const std::wstring sddl = std::wstring(L"D:P(A;;FA;;;") + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &descriptor, nullptr)) {
    return nullptr;
  }
  return descriptor;
}

Status WritePrivateFile(const fs::path& path, const std::string& contents) {
  const std::string name = PathString(path);
  PSECURITY_DESCRIPTOR descriptor = UserOnlySecurityDescriptor();
  if (descriptor == nullptr) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not restrict access to " + name);
  }
  // Create the file anew, so nothing already at the path, such as a link someone
  // who can write to the directory put there, is followed.
  if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
    LocalFree(descriptor);
    return Status(ADBC_STATUS_IO, "[motherduck] Could not replace " + name);
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  LocalFree(descriptor);
  if (file == INVALID_HANDLE_VALUE) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not create " + name);
  }
  DWORD written = 0;
  bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                      &written, nullptr) &&
            written == contents.size();
  ok = CloseHandle(file) && ok;
  if (!ok) return Status(ADBC_STATUS_IO, "[motherduck] Could not write " + name);
  return Status::Ok();
}
#else
std::optional<fs::path> EnvPath(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return fs::path(value);
}

std::optional<fs::path> MotherDuckHome() { return EnvPath("MOTHERDUCK_HOME"); }

std::optional<fs::path> HomeDirectory() {
  if (auto home = EnvPath("HOME")) return home;
  struct passwd entry = {};
  struct passwd* result = nullptr;
  std::vector<char> buffer(16 * 1024);
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 &&
      result != nullptr && result->pw_dir != nullptr && *result->pw_dir != '\0') {
    return fs::path(result->pw_dir);
  }
  return std::nullopt;
}

int ProcessId() { return static_cast<int>(getpid()); }

Status WritePrivateFile(const fs::path& path, const std::string& contents) {
  const std::string name = PathString(path);
  // Create the file anew, so nothing already at the path, such as a symlink
  // someone who can write to the directory put there, is followed.
  if (unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not replace " + name);
  }
  const int fd =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return Status(ADBC_STATUS_IO, "[motherduck] Could not create " + name);
  // Whatever the umask took away.
  bool ok = fchmod(fd, 0600) == 0;
  size_t written = 0;
  while (ok && written < contents.size()) {
    const ssize_t n = write(fd, contents.data() + written, contents.size() - written);
    if (n < 0 && errno == EINTR) continue;
    ok = n > 0;
    if (ok) written += static_cast<size_t>(n);
  }
  ok = (close(fd) == 0) && ok;
  if (!ok) return Status(ADBC_STATUS_IO, "[motherduck] Could not write " + name);
  return Status::Ok();
}
#endif

std::string_view Trim(std::string_view text) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  const size_t start = text.find_first_not_of(kWhitespace);
  if (start == std::string_view::npos) return {};
  return text.substr(start, text.find_last_not_of(kWhitespace) - start + 1);
}

constexpr std::chrono::milliseconds kLockPollInterval{100};

// How long to wait for a process holding the credentials lock without an
// announced login, without a login timeout: it's saving or deleting a token,
// which is quick.
constexpr std::chrono::seconds kUnannouncedHoldWait{30};
// How long past its announced end to wait for another process's login before
// deciding it's stuck.
constexpr std::chrono::seconds kAnnouncementGrace{10};
// After a login is denied, or finds no MotherDuck account, how long to fail
// instead of opening the browser again.
constexpr std::chrono::seconds kDeniedLoginBackoff{60};

Status WaitTimedOut() {
  return Status(ADBC_STATUS_TIMEOUT,
                "[motherduck] Timed out waiting for another MotherDuck login to finish");
}

// The process's logins: one at a time, and how the latest failed one ended.
struct SharedLogin {
  // Held while a TokenCache reads or changes its token, including while it logs
  // in, so that other connections wait for that login instead of starting one.
  std::mutex mutex;
  std::atomic<uint64_t> finished{0};
  // Guarded by mutex: the number (in finished) of the latest failed login, and
  // its failure.
  uint64_t failed = 0;
  AdbcStatusCode failure_code = ADBC_STATUS_OK;
  std::string failure_message;
};

SharedLogin& Shared() {
  static SharedLogin shared;
  return shared;
}

// Create the credentials directory, readable only by the user, if it's missing.
Status EnsureDirectory(const fs::path& directory) {
  std::error_code ec;
  if (fs::exists(directory, ec)) return Status::Ok();
  fs::create_directories(directory, ec);
  if (ec) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not create " +
                                      PathString(directory) + ": " + ec.message());
  }
  fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ec);
  return Status::Ok();
}

fs::path LockPath(const fs::path& path) {
  fs::path lock_path = path;
  lock_path += ".lock";
  return lock_path;
}

fs::path AnnouncementPath(const fs::path& path) {
  fs::path announcement = path;
  announcement += ".login";
  return announcement;
}

// Atomically replace the file at path, readable only by the user: write a
// sibling and rename it over the file, so readers never see a partial file.
Status ReplacePrivateFile(const fs::path& path, const std::string& contents) {
  std::error_code ec;
  const fs::path directory = path.parent_path();
  UNWRAP_STATUS(EnsureDirectory(directory));
  const fs::path temporary = directory / ("." + PathString(path.filename()) + "." +
                                          std::to_string(ProcessId()) + ".tmp");
  Status status = WritePrivateFile(temporary, contents);
  if (status.ok()) {
    fs::rename(temporary, path, ec);
    if (ec) {
      status = Status(ADBC_STATUS_IO, "[motherduck] Could not replace " +
                                          PathString(path) + ": " + ec.message());
    }
  }
  if (!status.ok()) fs::remove(temporary, ec);
  return status;
}

}  // namespace

#ifdef _WIN32
Status CredentialsLock::Acquire(const fs::path& path,
                                std::unique_ptr<CredentialsLock>* lock, bool block) {
  lock->reset();
  UNWRAP_STATUS(EnsureDirectory(path.parent_path()));
  const fs::path lock_path = LockPath(path);
  HANDLE file = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not open " + PathString(lock_path));
  }
  OVERLAPPED overlapped = {};
  const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (block ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
  if (!LockFileEx(file, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    if (!block && code == ERROR_LOCK_VIOLATION) return Status::Ok();
    return Status(ADBC_STATUS_IO, "[motherduck] Could not lock " + PathString(lock_path));
  }
  lock->reset(new CredentialsLock(reinterpret_cast<intptr_t>(file)));
  return Status::Ok();
}

// Closing the handle releases the lock.
CredentialsLock::~CredentialsLock() { CloseHandle(reinterpret_cast<HANDLE>(handle_)); }
#else
Status CredentialsLock::Acquire(const fs::path& path,
                                std::unique_ptr<CredentialsLock>* lock, bool block) {
  lock->reset();
  UNWRAP_STATUS(EnsureDirectory(path.parent_path()));
  const fs::path lock_path = LockPath(path);
  const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Status(ADBC_STATUS_IO, "[motherduck] Could not open " + PathString(lock_path));
  }
  while (flock(fd, LOCK_EX | (block ? 0 : LOCK_NB)) != 0) {
    const int code = errno;
    if (code == EINTR) continue;
    close(fd);
    if (!block && code == EWOULDBLOCK) return Status::Ok();
    return Status(ADBC_STATUS_IO, "[motherduck] Could not lock " + PathString(lock_path));
  }
  lock->reset(new CredentialsLock(fd));
  return Status::Ok();
}

// Closing the descriptor releases the lock.
CredentialsLock::~CredentialsLock() { close(static_cast<int>(handle_)); }
#endif

Status CredentialsPath(fs::path* path) {
  if (auto home = MotherDuckHome()) {
    // A relative path would silently put credentials somewhere unexpected.
    if (!home->is_absolute()) {
      return Status(ADBC_STATUS_INVALID_ARGUMENT,
                    "[motherduck] MOTHERDUCK_HOME must be an absolute path, got \"" +
                        PathString(*home) + "\"");
    }
    *path = *home / "credentials.json";
    return Status::Ok();
  }
  auto home = HomeDirectory();
  if (!home) {
    return Status(ADBC_STATUS_INVALID_STATE,
                  "[motherduck] Could not find the home directory for the MotherDuck "
                  "credentials file; set MOTHERDUCK_HOME");
  }
  *path = *home / ".motherduck" / "credentials.json";
  return Status::Ok();
}

std::optional<std::string> LoadSavedToken(const fs::path& path) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec || size > kMaxFileBytes) return std::nullopt;
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  const JsonObject credentials = JsonObject::Parse(text);
  if (credentials.GetInt("version") != 1) return std::nullopt;
  const std::optional<std::string> token = credentials.GetString("motherduck_token");
  if (!token) return std::nullopt;
  const std::string_view trimmed = Trim(*token);
  if (trimmed.empty()) return std::nullopt;
  return std::string(trimmed);
}

Status SaveToken(const fs::path& path, std::string_view token) {
  const std::optional<std::string> contents = CredentialsJson(token);
  if (!contents) {
    return Status(ADBC_STATUS_INVALID_ARGUMENT,
                  "[motherduck] Could not encode the MotherDuck token");
  }
  return ReplacePrivateFile(path, *contents);
}

Status AnnounceLogin(const fs::path& path, int64_t pid,
                     std::chrono::system_clock::time_point until) {
  const int64_t until_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(until.time_since_epoch())
          .count();
  return ReplacePrivateFile(AnnouncementPath(path),
                            std::to_string(pid) + " " + std::to_string(until_ms) + "\n");
}

std::optional<LoginAnnouncement> ReadLoginAnnouncement(const fs::path& path) {
  std::ifstream file(AnnouncementPath(path), std::ios::binary);
  if (!file) return std::nullopt;
  char buffer[64];
  file.read(buffer, sizeof(buffer));
  const std::string_view text = Trim(std::string_view(buffer, file.gcount()));
  const size_t space = text.find(' ');
  if (space == std::string_view::npos) return std::nullopt;
  int64_t pid = 0;
  int64_t until_ms = 0;
  const std::string_view pid_text = text.substr(0, space);
  const std::string_view until_text = text.substr(space + 1);
  const auto [pid_end, pid_ec] =
      std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
  const auto [until_end, until_ec] =
      std::from_chars(until_text.data(), until_text.data() + until_text.size(), until_ms);
  if (pid_ec != std::errc() || pid_end != pid_text.data() + pid_text.size() ||
      until_ec != std::errc() || until_end != until_text.data() + until_text.size() ||
      pid <= 0) {
    return std::nullopt;
  }
  LoginAnnouncement announcement;
  announcement.pid = pid;
  announcement.until =
      std::chrono::system_clock::time_point(std::chrono::milliseconds(until_ms));
  return announcement;
}

void RemoveLoginAnnouncement(const fs::path& path) {
  std::error_code ec;
  fs::remove(AnnouncementPath(path), ec);
}

int64_t CurrentProcessId() { return ProcessId(); }

#ifdef _WIN32
bool ProcessIsRunning(int64_t pid) {
  if (pid <= 0 || pid > MAXDWORD) return false;
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  // Access denied means it exists.
  if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return running;
}
#else
bool ProcessIsRunning(int64_t pid) {
  // kill(0) and negative IDs signal process groups, so only check real IDs.
  if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return false;
  // EPERM means it exists but belongs to another user.
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}
#endif

void RemoveSavedToken(const fs::path& path, std::string_view token) {
  if (LoadSavedToken(path) != token) return;
  std::error_code ec;
  fs::remove(path, ec);
}

uint64_t TokenCache::LoginsFinished() { return Shared().finished.load(); }

Status TokenCache::Get(bool need_host, Credentials* credentials, uint64_t since) {
  // Wait for a login in progress however long it takes: it ends by its own
  // timeout, and its token is the one this connection needs. Waiting only
  // login_timeout from now would give up just before a slow login finishes.
  std::lock_guard<std::mutex> lock(Shared().mutex);
  const Deadline deadline = LockDeadline();
  fs::path path;
  UNWRAP_STATUS(CredentialsPath(&path));

  // An expired token is used anyway: the server rejects it, and the caller logs
  // in again.
  bool fresh = false;
  if (token_.empty()) {
    std::optional<std::string> saved = LoadSavedToken(path);
    if (saved) {
      token_ = std::move(*saved);
    } else {
      UNWRAP_STATUS(Login(path, /*rejected=*/"", deadline, since, &fresh));
    }
  }

  if (need_host && host_.empty()) {
    Status status = LookupPgEndpoint(env_, options_, token_, &host_);
    if (!status.ok() && !fresh && status.ToAdbc(nullptr) == ADBC_STATUS_UNAUTHENTICATED) {
      // The saved token expired or was revoked.
      const std::string rejected = token_;
      ForgetLocked(path, deadline);
      UNWRAP_STATUS(Login(path, rejected, deadline, since, &fresh));
      status =
          host_.empty() ? LookupPgEndpoint(env_, options_, token_, &host_) : Status::Ok();
    }
    UNWRAP_STATUS(std::move(status));
  }

  credentials->token = token_;
  credentials->host = host_;
  credentials->fresh = fresh;
  return Status::Ok();
}

bool TokenCache::Reject(const std::string& token) {
  std::lock_guard<std::mutex> lock(Shared().mutex);
  const Deadline deadline = LockDeadline();
  if (token != token_) return true;
  // Keep the token if the API accepts it, or can't be reached: the failure has
  // another cause, which logging in again wouldn't fix.
  std::string pg_endpoint;
  if (LookupPgEndpoint(env_, options_, token, &pg_endpoint).ToAdbc(nullptr) !=
      ADBC_STATUS_UNAUTHENTICATED) {
    return false;
  }
  fs::path path;
  if (CredentialsPath(&path).ok()) {
    ForgetLocked(path, deadline);
  } else {
    token_.clear();
    host_.clear();
  }
  return true;
}

TokenCache::Deadline TokenCache::LockDeadline() const {
  if (options_.timeout <= std::chrono::seconds::zero()) return std::nullopt;
  return env_.now() + options_.timeout;
}

Status TokenCache::LockCredentials(const fs::path& path, const Deadline& deadline,
                                   std::unique_ptr<CredentialsLock>* lock) const {
  // A process that holds the lock without announcing a login is saving or
  // deleting a token: wait the login timeout for it, or a while without one.
  const auto unannounced_deadline =
      deadline ? *deadline : env_.now() + kUnannouncedHoldWait;
  while (true) {
    UNWRAP_STATUS(CredentialsLock::Acquire(path, lock, /*block=*/false));
    if (*lock != nullptr) return Status::Ok();
    // A process that's logging in announces until when. Wait for its login
    // however long it takes, since its token is the one to use, but no longer
    // than that: past it, the process is stuck.
    if (std::optional<LoginAnnouncement> login = ReadLoginAnnouncement(path);
        login && ProcessIsRunning(login->pid)) {
      if (std::chrono::system_clock::now() > login->until + kAnnouncementGrace) {
        return Status(ADBC_STATUS_TIMEOUT,
                      "[motherduck] Process " + std::to_string(login->pid) +
                          " has been logging in to MotherDuck for longer than its "
                          "timeout and still holds " +
                          PathString(LockPath(path)) + "; end it to log in here");
      }
    } else if (env_.now() >= unannounced_deadline) {
      return WaitTimedOut();
    }
    env_.sleep(kLockPollInterval);
  }
}

Status TokenCache::Login(const fs::path& path, const std::string& rejected,
                         const Deadline& deadline, uint64_t since, bool* fresh) {
  // Wait for a login another process is running through the same file, and use
  // its token. Without the lock, such as in a read-only home directory, log in
  // anyway.
  std::unique_ptr<CredentialsLock> lock;
  Status locked = LockCredentials(path, deadline, &lock);
  if (locked.ToAdbc(nullptr) == ADBC_STATUS_TIMEOUT) return locked;
  if (std::optional<std::string> saved = LoadSavedToken(path);
      saved && *saved != rejected) {
    token_ = std::move(*saved);
    host_.clear();
    *fresh = false;
    return Status::Ok();
  }

  // A login that ended during this connection attempt and failed (it timed
  // out, or the user denied it) answers for this attempt too: another browser
  // prompt would most likely end the same way.
  SharedLogin& shared = Shared();
  if (shared.failed > since) {
    return Status(shared.failure_code, shared.failure_message);
  }
  // After a login was denied, or found no MotherDuck account, another one would
  // most likely end the same way, so don't open the browser again for a while.
  // Timeouts and network errors don't count: the next login may well work.
  if (denied_until_ && env_.now() < *denied_until_) {
    return Status(ADBC_STATUS_UNAUTHENTICATED,
                  denied_message_ + " (not opening the browser again within " +
                      std::to_string(kDeniedLoginBackoff.count()) +
                      " seconds of that; initialize the database again to retry now)");
  }

  // Tell processes waiting for the lock how long this login may take.
  const bool announced =
      lock != nullptr &&
      AnnounceLogin(path, CurrentProcessId(),
                    std::chrono::system_clock::now() + MaxLoginDuration(options_))
          .ok();
  LoginResult result;
  Status status = DeviceLogin(env_, options_, &result);
  if (announced) RemoveLoginAnnouncement(path);
  const uint64_t number = ++shared.finished;
  if (!status.ok()) {
    shared.failed = number;
    shared.failure_code = status.ToAdbc(nullptr);
    shared.failure_message = status.message();
    if (shared.failure_code == ADBC_STATUS_UNAUTHENTICATED) {
      denied_until_ = env_.now() + kDeniedLoginBackoff;
      denied_message_ = status.message();
    }
    return status;
  }
  denied_until_.reset();
  token_ = std::move(result.token);
  host_ = std::move(result.pg_endpoint);
  *fresh = true;
  Status saved = SaveToken(path, token_);
  if (!saved.ok()) {
    env_.print(saved.message() + ". You'll be asked to log in again next time.");
  }
  return Status::Ok();
}

void TokenCache::ForgetLocked(const fs::path& path, const Deadline& deadline) {
  {
    // So another process can't save a new token between the check and the delete.
    // Without the lock, delete anyway; after waiting for it in vain, leave the
    // file to the process holding it.
    std::unique_ptr<CredentialsLock> lock;
    if (LockCredentials(path, deadline, &lock).ToAdbc(nullptr) != ADBC_STATUS_TIMEOUT) {
      RemoveSavedToken(path, token_);
    }
  }
  token_.clear();
  host_.clear();
}

}  // namespace adbcpq::auth
