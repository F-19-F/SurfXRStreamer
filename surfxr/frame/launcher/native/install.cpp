#include "controller.hpp"
#include "runtime_files.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace surfxr::launcher {
namespace {
std::vector<fs::path> data_roots(const Paths& paths) {
  std::vector<fs::path> roots{paths.config.parent_path(), paths.state, paths.home/".cache/surfxr-client"};
  const char* cache = std::getenv("XDG_CACHE_HOME");
  if (cache && *cache) roots.push_back(fs::path(cache)/"surfxr-client");
  return roots;
}
void validate_data_root(const fs::path& path) {
  if (!path.is_absolute() || path.filename() != "surfxr-client") throw std::runtime_error("purge_path_unverified");
  struct stat st{};
  if (::lstat(path.c_str(), &st)) {
    if (errno == ENOENT) return;
    throw std::runtime_error("purge_path_unverified");
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) throw std::runtime_error("purge_path_unverified");
}
void validate_root(const Paths& paths) {
  const auto expected = fs::weakly_canonical(paths.home) / ".local/share/surfxr-client";
  if (fs::is_symlink(paths.app) || fs::weakly_canonical(paths.app) != expected) throw std::runtime_error("installation_path_unverified");
  const auto shortcut = paths.home / ".local/bin/surfxr-client";
  if (fs::exists(shortcut) || fs::is_symlink(shortcut)) {
    if (!fs::is_symlink(shortcut) || fs::read_symlink(shortcut) != paths.app/"launcher/surfxr-client") throw std::runtime_error("installed_shortcut_unverified");
  }
}
void directory(const fs::path& path) {
  if (fs::is_symlink(path)) throw std::runtime_error("installation_directory_unverified");
  fs::create_directories(path);
}
std::string desktop_quote(const std::string& text) {
  std::string result = "\"";
  for (char c : text) { if (c == '\\' || c == '"' || c == '`' || c == '$') result += '\\'; if (c == '%') result += '%'; result += c; }
  return result + '"';
}
}
bool install_runtime(const Paths& paths, const fs::path& build) {
  validate_root(paths);
  directory(paths.app); directory(paths.app/"launcher"); directory(paths.app/"bin"); directory(paths.app/"licenses");
  for (auto name : {"surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-client", "surfxr-runtime-bridge"}) {
    const auto target = paths.app / (std::string(name) == "surfxr-client" ? "launcher" : "bin") / name;
    atomic_write(target, read_file(build/name, 32*1024*1024), 0755);
  }
  for (const auto* kind : {"lib", "licenses"}) {
    const auto source = build/"runtime"/kind;
    if (!fs::exists(source)) continue;
    directory(paths.app/kind);
    for (const auto& entry : fs::directory_iterator(source)) {
      if (!entry.is_regular_file() || entry.is_symlink()) throw std::runtime_error("invalid_runtime_payload");
      atomic_write(paths.app/kind/entry.path().filename(),read_file(entry.path(),32*1024*1024),0644);
    }
  }
  const bool input_driver=fs::is_regular_file(build/"driver_00_surfxr_exo_input.so");
  if(input_driver) {
    const auto driver=paths.app/"drivers/00_surfxr_exo_input";
    directory(paths.app/"drivers");directory(driver);directory(driver/"bin");directory(driver/"bin/linuxarm64");
    atomic_write(driver/"bin/linuxarm64/driver_00_surfxr_exo_input.so",read_file(build/"driver_00_surfxr_exo_input.so",8*1024*1024),0755);
    atomic_write(driver/"driver.vrdrivermanifest",input_driver_manifest,0644);
    configure_input_driver(paths,true);
  }
  atomic_write(paths.app/"licenses/sgsr1.LICENSE", image_license, 0644);
  atomic_write(paths.app/"licenses/NotoSans.LICENSE", font_license, 0644);
  auto shortcut = paths.home/".local/bin/surfxr-client";
  directory(shortcut.parent_path()); const auto temporary = shortcut.parent_path()/".surfxr-client-install";
  fs::remove(temporary); fs::create_symlink(paths.app/"launcher/surfxr-client",temporary); fs::rename(temporary,shortcut);
  auto services = paths.home/".config/systemd/user"; directory(services);
  atomic_write(services/"surfxr-client.service",client_service,0644);
  atomic_write(services/"surfxr-dashboard.service",dashboard_service,0644);
  auto desktop = paths.home/".local/share/applications/surfxr-client.desktop"; directory(desktop.parent_path());
  atomic_write(desktop,"[Desktop Entry]\nType=Application\nName=SurfXR\nComment=Connect to your PC desktop or SteamVR\nExec=" + desktop_quote(shortcut.string()) + " desktop\nIcon=video-display\nTerminal=false\nCategories=Game;Network;\nStartupNotify=false\n",0644);
  return input_driver;
}
Json configure_input_driver(const Paths& paths, bool enable) {
  validate_root(paths);
  const auto driver=paths.app/"drivers/00_surfxr_exo_input";
  if(enable && (!fs::is_regular_file(driver/"bin/linuxarm64/driver_00_surfxr_exo_input.so") ||
       fs::is_symlink(driver) || fs::is_symlink(driver/"bin") || fs::is_symlink(driver/"bin/linuxarm64")))
    throw std::runtime_error("frame_input_driver_not_installed");
  const auto result=run({"/opt/steamvr/bin/linuxarm64/vrpathreg",enable?"adddriver":"removedriver",driver.string()},10000);
  if(result.code)throw std::runtime_error("frame_input_driver_registration_failed");
  return object({{"registered",enable},{"steamvr_restart_required",true}});
}
void validate_purge(const Paths& paths) {
  validate_root(paths);
  for (const auto& root : data_roots(paths)) validate_data_root(root);
  validate_data_root(paths.runtime);
}
void remove_runtime(const Paths& paths, bool purge) {
  validate_root(paths);
  if (purge) validate_purge(paths); // Check all deletion roots before removing anything.
  if(fs::exists(paths.app/"drivers/00_surfxr_exo_input/driver.vrdrivermanifest") &&
     fs::is_regular_file("/opt/steamvr/bin/linuxarm64/vrpathreg"))configure_input_driver(paths,false);
  if (fs::exists(paths.app)) fs::remove_all(paths.app);
  fs::remove(paths.home/".local/bin/surfxr-client");
  fs::remove(paths.home/".local/share/applications/surfxr-client.desktop");
  fs::remove(paths.home/".config/systemd/user/surfxr-client.service");
  fs::remove(paths.home/".config/systemd/user/surfxr-dashboard.service");
  if (purge) {
    for (const auto& root : data_roots(paths)) fs::remove_all(root);
    if (fs::exists(paths.runtime)) for (const auto& entry : fs::directory_iterator(paths.runtime)) {
      const auto name = entry.path().filename();
      // Stable lock inodes exclude concurrent/manual owners, including reinstalls.
      // They hold no settings and disappear with the OS user runtime at logout.
      if (name != "service.lock" && name != "viewer.lock" && name != "overlay.lock" &&
          name != "input-driver.lock" && name != "system-input.sock") fs::remove_all(entry.path());
    }
  }
}
}
