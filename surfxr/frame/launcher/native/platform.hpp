#pragma once
#include "json.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <sys/types.h>

namespace surfxr::launcher {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
struct Paths {
  fs::path home, app, config, state, runtime, viewer;
  static Paths user();
};
class Fd {
 public:
  explicit Fd(int value = -1) : value_(value) {}
  ~Fd();
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return value_; }
 private: int value_;
};
class Lock {
 public:
  explicit Lock(const fs::path& file);
 private: Fd fd_;
};
void private_directory(const fs::path& path);
std::string read_file(const fs::path& path, size_t limit, bool private_file = false);
void atomic_write(const fs::path& path, const std::string& data, unsigned mode = 0600);
std::string sha256(const fs::path& path);
fs::path executable();
class Process {
 public:
  Process(const std::vector<std::string>& args, const std::string& input = "", size_t output_limit = 65536);
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  bool poll();
  void signal(int kind);
  std::string output() const;
  pid_t pid() const { return pid_; }
  int code() const { return code_; }
  bool oversized() const { return oversized_; }
 private:
  Fd output_;
  pid_t pid_ = -1;
  int code_ = -1;
  bool done_ = false, oversized_ = false;
  size_t limit_;
};
struct CommandResult { int code; std::string output; };
CommandResult run(const std::vector<std::string>& args, int milliseconds = 1000);
Json ipc(const Paths& paths, const Json& request);
void viewer_command(const Paths& paths, const std::string& command);
int desktop_window(const Paths& paths);
}
