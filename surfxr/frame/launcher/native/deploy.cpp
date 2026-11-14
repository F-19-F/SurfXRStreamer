#include "controller.hpp"
#include "session_failure.hpp"
#include "surfxr/build_features.hpp"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <regex>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace surfxr::launcher {
namespace {
Json status(const Paths& paths) { try { return ipc(paths,object({{"command","status"}})); } catch (...) { return {}; } }
void systemctl(const std::vector<std::string>& arguments, bool check = true) {
  const auto runtime = "/run/user/" + std::to_string(::getuid());
  ::setenv("XDG_RUNTIME_DIR",runtime.c_str(),0);
  const auto bus = "unix:path=" + std::string(std::getenv("XDG_RUNTIME_DIR")) + "/bus";
  ::setenv("DBUS_SESSION_BUS_ADDRESS",bus.c_str(),0);
  std::vector<std::string> argv = {"systemctl","--user"}; argv.insert(argv.end(),arguments.begin(),arguments.end());
  if (run(argv,60000).code && check) throw std::runtime_error("frame_user_service_failed");
}
void stop(const Paths& paths) {
  auto current = status(paths);
  if (!current["pid"].is(json_type_null)) {
    if (!ipc(paths,object({{"command","stop"}}))["ok"].boolean()) throw std::runtime_error("viewer_stop_rejected");
    const auto deadline = Clock::now()+std::chrono::seconds(25);
    do { current = status(paths); if (!current.is(json_type_null) && current["pid"].is(json_type_null)) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); } while (Clock::now() < deadline);
    if (current.is(json_type_null) || !current["pid"].is(json_type_null)) throw std::runtime_error("viewer_stop_timeout");
  }
  private_directory(paths.runtime); Lock viewer(paths.runtime/"viewer.lock");
}
void retire_dashboard(const Paths& paths) {
  Fd fd(::open((paths.runtime/"overlay.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));
  if (fd.get() < 0) throw std::runtime_error("dashboard_lock_failed");
  if (!::flock(fd.get(),LOCK_EX|LOCK_NB)) return;
  if (errno != EWOULDBLOCK && errno != EAGAIN) throw std::runtime_error("dashboard_lock_failed");
  char text[32]{}; const auto count = ::pread(fd.get(),text,sizeof(text)-1,0);
  if (count <= 0 || !std::regex_match(std::string(text,static_cast<size_t>(count)),std::regex("[0-9]+\\n?"))) throw std::runtime_error("dashboard_owner_identity_unverified");
  const auto pid = std::stol(text); if (pid <= 1 || pid > INT32_MAX) throw std::runtime_error("dashboard_owner_identity_unverified");
  Fd handle(static_cast<int>(::syscall(SYS_pidfd_open,pid,0)));
  if (handle.get() < 0 && errno != ESRCH) throw std::runtime_error("dashboard_owner_identity_unverified");
  if (handle.get() >= 0) {
    const auto process = fs::path("/proc")/std::to_string(pid); auto target = fs::read_symlink(process/"exe").string();
    const std::string suffix = " (deleted)"; if (target.size() >= suffix.size() && target.compare(target.size()-suffix.size(),suffix.size(),suffix) == 0) target.resize(target.size()-suffix.size());
    struct stat st{};
    if (::stat(process.c_str(),&st) || st.st_uid != ::getuid() || target != fs::weakly_canonical(paths.app/"bin/surfxr-dashboard").string()) throw std::runtime_error("dashboard_owner_identity_unverified");
    if (::syscall(SYS_pidfd_send_signal,handle.get(),SIGTERM,nullptr,0) && errno != ESRCH) throw std::runtime_error("dashboard_stop_failed");
  }
  const auto deadline = Clock::now()+std::chrono::seconds(10);
  while (::flock(fd.get(),LOCK_EX|LOCK_NB)) {
    if (Clock::now() >= deadline) throw std::runtime_error("dashboard_stop_timeout");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
void open_dashboard(const Paths& paths) {
  try { systemctl({"start","steamvr.service"}); }
  catch (...) { throw std::runtime_error("frame_dashboard_runtime_start_failed"); }
  try { systemctl({"start","surfxr-dashboard.service"}); }
  catch (...) { throw std::runtime_error("frame_dashboard_service_start_failed"); }
  Fd socket(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
  if (socket.get() < 0) throw std::runtime_error("frame_dashboard_socket_failed");
  const auto endpoint = (paths.runtime/"overlay.sock").string();
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(address.sun_path)) throw std::runtime_error("frame_dashboard_socket_failed");
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);
  const auto deadline = Clock::now()+std::chrono::seconds(10);
  do {
    struct stat st{};
    if (!::lstat(endpoint.c_str(),&st)) {
      if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0077))
        throw std::runtime_error("frame_dashboard_socket_unverified");
      if (::sendto(socket.get(),"show",4,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == 4) return;
    } else if (errno != ENOENT) throw std::runtime_error("frame_dashboard_socket_failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  } while (Clock::now() < deadline);
  throw std::runtime_error("frame_dashboard_not_ready");
}
Json setup(const Paths& paths, const Json& request, bool release, bool = true) {
  const auto link = request["link"].text("direct"); if (link != "lan" && link != "direct") throw std::runtime_error("invalid_link");
  auto data = fs::exists(paths.config) || fs::is_symlink(paths.config) ? read_connection(paths) : new_connection(link);
  data.set("backend","surfxr");
  if (!release) {
    auto endpoints = data["profiles"], last = data["last_hosts"];
    for (auto name : {"lan", "direct"}) {
      auto endpoint = object({{"host", ipv4(request[std::string("pc_")+name],true)}, {"local_address", request.has(std::string("frame_")+name) ? ipv4(request[std::string("frame_")+name],true) : ""}});
      endpoints.set(name,endpoint); if (!endpoint["host"].text().empty()) last.set(name,"");
    }
    endpoints.set("auto",endpoints["lan"]); data.set("profiles",endpoints); data.set("profile",link); data.set("last_hosts",last);
  }
  return normalize_connection(data);
}
}
Json verify_release(const fs::path& root) {
  auto manifest = Json::parse(read_file(root/"release-manifest.json",65536));
  utsname machine{}; ::uname(&machine);
  if (manifest["format_version"].integer() != 2 || manifest["architecture"].text() != "aarch64" || std::string(machine.machine) != "aarch64" ||
      !manifest["frame_python_required"].is(json_type_boolean) || manifest["frame_python_required"].boolean()) throw std::runtime_error("incompatible_frame_release");
  for (const auto& name : manifest["files"].keys()) {
    if (name.rfind("payload/",0) != 0) continue;
    const auto relative = name.substr(8);
    if (!std::regex_match(relative,std::regex("build/(surfxr-(viewer|dashboard|discover|client|runtime-bridge)|driver_00_surfxr_exo_input\\.so|runtime/(lib/lib[a-z0-9_]+\\.so\\.[0-9]+|licenses/[A-Za-z0-9_.-]+))"))) throw std::runtime_error("invalid_release_path");
    if (sha256(root/relative) != manifest["files"][name].text()) throw std::runtime_error("frame_release_hash_mismatch");
  }
  std::vector<const char*> binaries{"surfxr-viewer","surfxr-dashboard","surfxr-discover","surfxr-client","surfxr-runtime-bridge"};
  if(manifest["files"].has("payload/build/driver_00_surfxr_exo_input.so"))binaries.push_back("driver_00_surfxr_exo_input.so");
  for (auto name : binaries) {
    if (!manifest["files"].has(std::string("payload/build/")+name)) throw std::runtime_error("frame_release_binary_missing");
    ::chmod((root/"build"/name).c_str(),0755);
  }
  return object({{"verified",true}});
}
Json deployment(const Paths& paths, const std::string& command, const Json& request) {
  const auto build = executable().parent_path();
  if (command == "verify-release") return verify_release(build.parent_path());
  if (command == "input-driver-register" || command == "input-driver-unregister")
    return configure_input_driver(paths,command == "input-driver-register");
  if (command == "inspect") {
    return object({{"state",status(paths)["state"].text("stopped")},{"backend","surfxr"}});
  }
  if (command == "stop") { stop(paths); return object({{"state","idle"}}); }
  if (command == "start") {
    systemctl({"start","steamvr.service"}); systemctl({"start","surfxr-dashboard.service"});
    auto response = ipc(paths,object({{"command","start"},{"mode",request["mode"].text("desktop")},
                                     {"profile",request["link"].text()}}));
    if (!response["ok"].boolean()) throw std::runtime_error("frame_start_rejected_check_launcher_status");
    return observe_start([&paths] { return status(paths); },request["mode"].text("desktop"));
  }
  const bool purge = command == "purge", uninstall = command == "uninstall" || purge;
  if (command != "install" && command != "deploy" && !uninstall) throw std::runtime_error("invalid_deployment_command");
  if (purge) validate_purge(paths);
  Json data; if (!uninstall) data = setup(paths,request,command == "install");
  stop(paths); systemctl({"stop","surfxr-dashboard.service","surfxr-client.service"},false); retire_dashboard(paths);
  Json installed;
  {
    Lock viewer(paths.runtime/"viewer.lock"), dashboard(paths.runtime/"overlay.lock");
    if (uninstall) {
      systemctl({"disable","surfxr-dashboard.service","surfxr-client.service"},false);
      if (purge) { Lock service(paths.runtime/"service.lock"); remove_runtime(paths,true); }
      else remove_runtime(paths);
      systemctl({"daemon-reload"});
      return object({{"uninstalled",true},{"configuration_preserved",!purge},{"state","stopped"}});
    }
    const bool input_driver=install_runtime(paths,build); write_connection(paths,data); systemctl({"daemon-reload"});
    systemctl({"enable","surfxr-client.service","surfxr-dashboard.service"}); systemctl({"restart","surfxr-client.service"});
    const auto deadline = Clock::now()+std::chrono::seconds(10);
    while (Clock::now() < deadline) {
      if (!status(paths).is(json_type_null)) { auto config = ipc(paths,object({{"command","config_get"}}));
        if (!config["ok"].boolean() || (command == "deploy" && !config["connection_config"]["effective"]["ready"].boolean())) throw std::runtime_error("frame_selected_links_unavailable_check_addresses");
        installed = object({{"installed",true},{"state","idle"},{"backend",data["backend"]},{"configured",config["configured"]}});
        installed.set("input_driver_registered",input_driver);
        installed.set("steamvr_restart_required",input_driver);
        break;
      } std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (installed.is(json_type_null)) throw std::runtime_error("frame_connection_service_not_ready");
  if (command == "install") {
    try { open_dashboard(paths); installed.set("dashboard_started",true); }
    catch (const std::exception& error) {
      const std::string reason = error.what();
      installed.set("dashboard_started",false);
      installed.set("dashboard_error",reason.rfind("frame_dashboard_",0) == 0 &&
          std::regex_match(reason,std::regex("[a-z_]{1,128}")) ? reason : "frame_dashboard_start_failed");
    }
  }
  return installed;
}
}
