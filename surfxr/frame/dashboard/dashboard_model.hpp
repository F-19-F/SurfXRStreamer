#pragma once
#include "surfxr/build_features.hpp"
#include "surfxr/steamvr_startup.hpp"
#include "surfxr/remote_settings.hpp"
#include "surfxr/input_preferences.hpp"
// A small dashboard process. Streaming and connection ownership live in the
#include <openvr.h>
#include <cairo/cairo.h>
#include <json-c/json.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace surfxr::dashboard {
using Clock = std::chrono::steady_clock;
constexpr int width = 1280, height = 840;
constexpr const char* overlay_key = "org.steamframe.surfxr.dashboard";
inline volatile std::sig_atomic_t stop_requested = 0;
inline void stop_signal(int) { stop_requested = 1; }

class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() { if (fd_ >= 0) ::close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
 private:
  int fd_;
};

inline sockaddr_un address(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path))
    throw std::runtime_error("The runtime socket path is too long.");
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  return addr;
}

inline std::string runtime_directory() {
  const char* env = std::getenv("XDG_RUNTIME_DIR");
  std::string root = env && *env ? env : "/run/user/" + std::to_string(::getuid());
  struct stat st{};
  if (root.empty() || root[0] != '/' || ::lstat(root.c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0022))
    throw std::runtime_error("A private user runtime directory is required.");
  root += "/surfxr-client";
  if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("Cannot create the dashboard runtime directory.");
  if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::getuid() || (st.st_mode & 0077))
    throw std::runtime_error("The dashboard runtime directory must be private.");
  return root;
}

// flock stays on a stable inode; do not unlink its file on shutdown. A second
// invocation sends a harmless wakeup to the lock owner's private datagram socket.
class Singleton {
 public:
  explicit Singleton(const std::string& directory)
      : path_(directory + "/overlay.sock"),
        lock_(::open((directory + "/overlay.lock").c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)),
        socket_(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) {
    struct stat st{};
    if (lock_.get() < 0 || socket_.get() < 0 || ::fstat(lock_.get(), &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_uid != ::getuid())
      throw std::runtime_error("Cannot open the dashboard singleton lock.");
    if (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) return;
      throw std::runtime_error("Cannot lock the dashboard instance.");
    }
    owns_ = true;
    if (::lstat(path_.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
        throw std::runtime_error("An unexpected file occupies the dashboard socket.");
      if (::unlink(path_.c_str()) != 0)
        throw std::runtime_error("Cannot replace the old dashboard socket.");
    } else if (errno != ENOENT) {
      throw std::runtime_error("Cannot inspect the dashboard socket.");
    }
    auto addr = address(path_);
    if (::bind(socket_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::chmod(path_.c_str(), 0600) != 0)
      throw std::runtime_error("Cannot create the dashboard wakeup socket.");
    bound_ = true;
    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(lock_.get(), 0) == 0)
      (void)::write(lock_.get(), pid.data(), pid.size());
  }
  ~Singleton() { if (bound_) (void)::unlink(path_.c_str()); }
  bool owns() const { return owns_; }
  bool show_existing() const {
    const auto addr = address(path_);
    // A simultaneous service and desktop launch can arrive before bind finishes.
    for (int attempt = 0; attempt != 10; ++attempt) {
      if (::sendto(socket_.get(), "show", 4, MSG_NOSIGNAL,
                   reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 4)
        return true;
      if (errno != ENOENT && errno != ECONNREFUSED) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
  }
  bool take_show() const {
    bool show = false;
    char bytes[16];
    for (int count = 0; count != 16; ++count) {
      const ssize_t n = ::recv(socket_.get(), bytes, sizeof(bytes), MSG_DONTWAIT);
      if (n < 0) break;
      show = show || (n == 4 && std::memcmp(bytes, "show", 4) == 0);
    }
    return show;
  }
 private:
  std::string path_;
  Fd lock_, socket_;
  bool owns_ = false, bound_ = false;
};

struct ConnectionChoice {
  std::string host, local_address, interface, kind;
};
struct ConnectionConfig {
  std::string backend = "surfxr", surfxr_version = "21.0.0-dev14";
  bool backend_supported = false;
  bool loaded = false, ready = false;
  bool discovering = false;
  bool remember_last_ip = true;
  std::array<std::string, 3> last_hosts;
  std::string discovery_message;
  int distributor_port = 39850;
  std::string signature, profile = "auto", reason, transport = "tcp", video_strategy = "legacy";
  std::array<ConnectionChoice, 3> profiles;
  std::vector<ConnectionChoice> candidates;
};
constexpr std::array<const char*, 3> profile_names{{"auto", "lan", "direct"}};
inline size_t profile_index(const std::string& name) {
  for (size_t i = 0; i != profile_names.size(); ++i) if (name == profile_names[i]) return i;
  return 0;
}

struct QualitySettings {
  bool loaded = false, editable = false, gaze_editable = false;
  std::string signature, message;
  std::array<double, 6> values{{default_remote_setting(SettingId::VrGraphicsQuality),
    default_remote_setting(SettingId::VrFramerate),default_remote_setting(SettingId::VrBitrateMbps),
    default_remote_setting(SettingId::FoveatedStreaming),0,default_remote_setting(SettingId::DesktopPointerPriority)}};
  std::array<std::vector<double>, 6> choices;
  bool gaze_available=false;
  std::string gaze_state="waiting";
  unsigned focus_width=0,focus_height=0,eye_width=0,eye_height=0,base_width=0,base_height=0;
  std::array<double,2> center_x{},center_y{};
};

struct InputPreferencesPanel : InputPreferences {
  bool loaded = false, editable = false;
  std::string signature, message;
};

enum class SettingsPage { Stream, Image, Devices, Diagnostics };
enum class DiagnosticsPage { Overlays, Content, Layout, Gaze };

struct Status {
  bool available = false, configured = false, ok = false;
  std::string state = "offline", message = "The connection service is unavailable.";
  std::string host, mode = "desktop", pending_mode, backend = "surfxr";
  std::string steamvr_startup_state,steamvr_startup_reason;
  std::uint64_t steamvr_startup_request_id=0;
  std::string failure_code, failure_title, failure_detail, failure_action;
  int64_t failure_exit = 0;
  ConnectionConfig connection;
  QualitySettings quality;
  QualitySettings image_quality;
  InputPreferencesPanel input_preferences;
  int raw_battery_percent=-1;
  bool raw_battery_charging=false;
  SettingsPage settings_page = SettingsPage::Stream;
  DiagnosticsPage diagnostics_page = DiagnosticsPage::Overlays;
  int64_t pid = 0;
  int healthy_links = -1;
  std::string active_transport,active_video_strategy;
  bool operator==(const Status& b) const {
    return raw_battery_percent==b.raw_battery_percent&&raw_battery_charging==b.raw_battery_charging&&
           available == b.available && configured == b.configured && ok == b.ok &&
           state == b.state && message == b.message && host == b.host &&
           failure_code == b.failure_code && failure_title == b.failure_title && failure_detail == b.failure_detail &&
           failure_action == b.failure_action && failure_exit == b.failure_exit &&
           mode == b.mode && backend == b.backend && pending_mode == b.pending_mode && pid == b.pid && healthy_links == b.healthy_links && active_transport == b.active_transport && active_video_strategy==b.active_video_strategy &&
           steamvr_startup_state==b.steamvr_startup_state&&steamvr_startup_reason==b.steamvr_startup_reason&&steamvr_startup_request_id==b.steamvr_startup_request_id&&
           connection.signature == b.connection.signature && quality.signature == b.quality.signature &&
           image_quality.signature == b.image_quality.signature && input_preferences.signature == b.input_preferences.signature && settings_page == b.settings_page && diagnostics_page == b.diagnostics_page;
  }
  bool can_start() const {
    return available && configured && (state == "idle" || state == "error");
  }
  bool can_select(const char* target) const {
    if (backend == "surfxr" && std::strcmp(target,"vr") != 0 && connection.surfxr_version!="21.0.0-dev14") return false;
    if (can_start()) return true;
    return available && (state == "starting" || state == "streaming") &&
           (pending_mode.empty() ? mode : pending_mode) != target;
  }
  bool can_stop() const { return state == "starting" || state == "streaming"; }
};

// Lightweight status replies omit configuration. Keep the last complete
// snapshot without replacing it with defaults after every status poll.
inline Status merge_status(Status updated, const Status& previous) {
  updated.settings_page = previous.settings_page;
  updated.diagnostics_page = previous.diagnostics_page;
  // Battery is sampled locally even while the connection service is idle.
  updated.raw_battery_percent=previous.raw_battery_percent;
  updated.raw_battery_charging=previous.raw_battery_charging;
  if (updated.available) {
    if (!updated.connection.loaded) updated.connection = previous.connection;
    if (!updated.quality.loaded) updated.quality = previous.quality;
    if (!updated.image_quality.loaded) updated.image_quality = previous.image_quality;
    if (!updated.input_preferences.loaded) updated.input_preferences = previous.input_preferences;
  }
  return updated;
}

inline std::string display_text(const char* value, size_t limit) {
  // Only fixed messages and host names are expected; reject control characters
  // and invalid UTF-8 before handing text to Cairo's toy font API.
  std::string out;
  if (!value) return out;
  for (; *value && out.size() < limit; ++value) {
    const auto c = static_cast<unsigned char>(*value);
    out += c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
  }
  return out;
}

inline json_object* member(json_object* object, const char* name, json_type type) {
  json_object* value = nullptr;
  return object && json_object_object_get_ex(object, name, &value) && value &&
         json_object_is_type(value, type) ? value : nullptr;
}
inline std::string string_member(json_object* object, const char* name, size_t limit = 96) {
  return display_text(json_object_get_string(member(object, name, json_type_string)), limit);
}


Status parse_status(const std::string&);
Status request_payload(const std::string&,const std::string&);
Status request(const std::string&,const char*,const char* mode="desktop");
Status save_connection(const std::string&,const char*,const ConnectionChoice&);
Status save_video_strategy(const std::string&,const ConnectionConfig&,const char*);
Status save_quality(const std::string&,size_t,double,bool local=false);
std::string input_preference_payload(unsigned,int);
Status save_input_preference(const std::string&,int,unsigned id=0);
Status example(const std::string&);
void self_test();
int run(bool);

} // namespace surfxr::dashboard
