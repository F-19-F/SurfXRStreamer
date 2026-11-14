#include "platform.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace surfxr::launcher {
namespace {
fs::path environment_path(const char* name, const fs::path& fallback) {
  const char* value = std::getenv(name);
  fs::path result = value && *value ? fs::path(value) : fallback;
  if (!result.is_absolute()) throw std::runtime_error("absolute_user_path_required");
  return result;
}
void write_all(int fd, const std::string& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const auto n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("private_write_failed");
    offset += static_cast<size_t>(n);
  }
}
int temporary_output() {
  auto* file = ::tmpfile();
  if (!file) throw std::runtime_error("capture_failed");
  int fd = ::fcntl(::fileno(file), F_DUPFD_CLOEXEC, 3);
  ::fclose(file);
  if (fd < 0) throw std::runtime_error("capture_failed");
  return fd;
}
}
Fd::~Fd() { if (value_ >= 0) ::close(value_); }
Paths Paths::user() {
  Paths p;
  p.home = environment_path("HOME", {});
  p.app = p.home / ".local/share/surfxr-client";
  p.config = environment_path("XDG_CONFIG_HOME", p.home / ".config") / "surfxr-client/connection.json";
  p.state = environment_path("XDG_STATE_HOME", p.home / ".local/state") / "surfxr-client";
  p.runtime = environment_path("XDG_RUNTIME_DIR", "/run/user/" + std::to_string(::getuid())) / "surfxr-client";
  p.viewer = p.app / "bin/surfxr-viewer";
  return p;
}
void private_directory(const fs::path& path) {
  if (!fs::exists(path)) { fs::create_directories(path); if (::chmod(path.c_str(), 0700)) throw std::runtime_error("private_directory_failed"); }
  struct stat st{};
  if (::lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0077))
    throw std::runtime_error("directory_not_private");
}
Lock::Lock(const fs::path& file) : fd_(::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)) {
  struct stat st{};
  if (fd_.get() < 0 || ::fstat(fd_.get(), &st) || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0077))
    throw std::runtime_error("lock_not_private");
  if (::flock(fd_.get(), LOCK_EX | LOCK_NB)) throw std::runtime_error("instance_active");
}
std::string read_file(const fs::path& path, size_t limit, bool private_file) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat st{};
  if (fd.get() < 0 || ::fstat(fd.get(), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) > limit ||
      (private_file && (st.st_uid != ::getuid() || (st.st_mode & 0077)))) throw std::runtime_error("invalid_private_file");
  std::string data;
  std::array<char,4096> buffer{};
  for (;;) {
    const auto n = ::read(fd.get(), buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) throw std::runtime_error("file_read_failed");
    if (!n) break;
    data.append(buffer.data(), static_cast<size_t>(n));
    if (data.size() > limit) throw std::runtime_error("file_too_large");
  }
  return data;
}
void atomic_write(const fs::path& path, const std::string& data, unsigned mode) {
  fs::create_directories(path.parent_path());
  auto name = (path.parent_path() / ("." + path.filename().string() + ".XXXXXX")).string();
  std::vector<char> pattern(name.begin(), name.end()); pattern.push_back(0);
  Fd fd(::mkstemp(pattern.data()));
  if (fd.get() < 0) throw std::runtime_error("atomic_write_failed");
  try {
    if (::fchmod(fd.get(), mode)) throw std::runtime_error("file_permissions_failed");
    write_all(fd.get(), data);
    if (::fsync(fd.get()) || ::rename(pattern.data(), path.c_str())) throw std::runtime_error("atomic_write_failed");
  } catch (...) { ::unlink(pattern.data()); throw; }
}
std::string sha256(const fs::path& path) {
  // Release files are bounded; no arbitrary evidence or payload is read.
  const auto data = read_file(path, 32 * 1024 * 1024);
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned count = 0;
  if (!EVP_Digest(data.data(), data.size(), bytes.data(), &count, EVP_sha256(), nullptr)) throw std::runtime_error("hash_failed");
  std::string result;
  for (unsigned i = 0; i < count; ++i) { result += "0123456789abcdef"[bytes[i] >> 4]; result += "0123456789abcdef"[bytes[i] & 15]; }
  return result;
}
fs::path executable() { return fs::canonical("/proc/self/exe"); }
Process::Process(const std::vector<std::string>& args, const std::string& input, size_t limit)
    : output_(temporary_output()), limit_(limit) {
  if (args.empty() || input.size() > 8192) throw std::runtime_error("invalid_child_request");
  int pipefd[2];
  if (::pipe2(pipefd, O_CLOEXEC)) throw std::runtime_error("child_pipe_failed");
  Fd reader(pipefd[0]), writer(pipefd[1]);
  std::vector<char*> argv;
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  pid_ = ::fork();
  if (pid_ < 0) throw std::runtime_error("child_start_failed");
  if (!pid_) {
    ::setsid();
    ::dup2(reader.get(), STDIN_FILENO); ::dup2(output_.get(), STDOUT_FILENO);
    int null = ::open("/dev/null", O_WRONLY); ::dup2(null, STDERR_FILENO); ::close(null);
    ::execvp(argv[0], argv.data()); ::_exit(127);
  }
  try { write_all(writer.get(), input); }
  catch (...) { signal(SIGKILL); while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {} done_ = true; throw; }
}
Process::~Process() {
  if (!done_ && pid_ > 0) { signal(SIGKILL); while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {} }
}
bool Process::poll() {
  if (done_) return true;
  struct stat st{};
  if (!::fstat(output_.get(), &st) && st.st_size > static_cast<int64_t>(limit_)) {
    oversized_ = true;
    if (!::ftruncate(output_.get(), 0)) {
      const auto offset = ::lseek(output_.get(), 0, SEEK_SET);
      (void)offset;
    }
  }
  int status = 0;
  const auto result = ::waitpid(pid_, &status, WNOHANG);
  if (result == pid_) { done_ = true; code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status); }
  return done_;
}
void Process::signal(int kind) {
  if (!done_ && pid_ > 0 && ::kill(-pid_, kind) && errno == ESRCH) (void)::kill(pid_, kind);
}
std::string Process::output() const {
  struct stat st{};
  if (::fstat(output_.get(), &st) || st.st_size > static_cast<int64_t>(limit_) || oversized_) throw std::runtime_error("capture_too_large");
  std::string result(static_cast<size_t>(st.st_size), '\0');
  const auto n = ::pread(output_.get(), result.data(), result.size(), 0);
  if (n < 0) throw std::runtime_error("capture_read_failed");
  result.resize(static_cast<size_t>(n)); return result;
}
CommandResult run(const std::vector<std::string>& args, int milliseconds) {
  Process child(args);
  const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
  while (!child.poll() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!child.poll()) throw std::runtime_error("command_timeout");
  return {child.code(), child.output()};
}
}
