#include "native_fixture.hpp"
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <thread>

namespace surfxr::launcher_test {
using namespace surfxr::launcher;
namespace {
void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
CommandResult execute(const std::vector<std::string>& args, const std::string& input) {
  Process child(args,input);
  const auto deadline = Clock::now()+std::chrono::seconds(5);
  while (!child.poll() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(child.poll(),"removal_fixture_timeout");
  return {child.code(),child.output()};
}
}
void deployment_checks(const fs::path& home) {
  fs::create_directory(home);
  const auto fixtures = home/"fake-tools";
  fs::create_directory(fixtures);
  fs::copy_file(executable(),fixtures/"systemctl");
  const auto binary = read_file(executable().parent_path()/"surfxr-client",32*1024*1024);
  const auto script = read_file(SURFXR_RESIDUAL_REMOVE_SCRIPT,65536);
  const auto app = home/".local/share/surfxr-client";
  const auto native = app/"launcher/surfxr-client";
  const auto config = home/".config/surfxr-client";
  const auto state = home/".local/state/surfxr-client";
  const auto cache = home/".cache/surfxr-client";
  const auto xdg_cache = home/"xdg-cache/surfxr-client";
  const auto runtime = home/"runtime/surfxr-client";
  const auto external = home/"external";
  // The real vrpathreg is confined to this fixture's path registry, even if
  // the invoking SteamVR environment supplied its own override.
  const auto registry = home/"openvrpaths.vrpath";
  const bool driver_tool=fs::is_regular_file("/opt/steamvr/bin/linuxarm64/vrpathreg");
  atomic_write(registry,R"({"jsonid":"vrpathreg","version":1,"external_drivers":[]})");
  atomic_write(external/"kept","other application");
  atomic_write(home/"xdg-cache/other-app/kept","other cache");
  const std::vector<std::string> environment = {"/usr/bin/env","HOME="+home.string(),
      "XDG_CONFIG_HOME="+(home/".config").string(),"XDG_STATE_HOME="+(home/".local/state").string(),
      "XDG_CACHE_HOME="+(home/"xdg-cache").string(),"XDG_RUNTIME_DIR="+(home/"runtime").string(),
      "DBUS_SESSION_BUS_ADDRESS=unix:path="+(home/"runtime/bus").string(),
      "VR_PATHREG_OVERRIDE="+registry.string(),
      "PATH="+fixtures.string()+":"+(std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin")};
  auto invoke = [&](const std::vector<std::string>& command, const std::string& input, bool success,
                    const char* label, const char* error = "") {
    auto args = environment; args.insert(args.end(),command.begin(),command.end());
    const auto result = execute(args,input);
    const auto response = Json::parse(result.output);
    require((result.code == 0) == success && response["ok"].boolean() == success,label);
    if (!success) require(response["error"].text() == error,label);
    return response;
  };
  auto seed_data = [&] {
    atomic_write(config/"connection.json","fixture configuration");
    atomic_write(state/"settings.conf","fixture preferences");
    atomic_write(state/"last-session.json","fixture report");
    atomic_write(cache/"old-release","fixture cache");
    atomic_write(xdg_cache/"entry","fixture cache");
    fs::create_directory_symlink(external,state/"external-link");
  };
  auto removed = [&] {
    require(!fs::exists(app) && !fs::exists(config) && !fs::exists(state) && !fs::exists(cache) &&
            !fs::exists(xdg_cache),"cli_purge_removed_all_owned_data");
    require(fs::exists(external/"kept") && fs::exists(home/"xdg-cache/other-app/kept"),"cli_purge_keeps_siblings_and_link_targets");
    require(fs::exists(runtime/"input-driver.lock")&&fs::exists(runtime/"system-input.sock"),"purge_preserves_driver_runtime_until_vrserver_cleanup");
  };
  seed_data(); atomic_write(native,binary,0755);
  auto response = invoke({native.string(),"--deploy","uninstall"},"{}",true,"installed_cli_uninstall_can_delete_its_own_executable");
  require(response["configuration_preserved"].boolean() && !fs::exists(app) && fs::exists(config/"connection.json"),
          "installed_cli_uninstall_preserves_settings");
  atomic_write(runtime/"input-driver.lock","stable driver lock");
  atomic_write(runtime/"system-input.sock","fixture driver endpoint");
  response = invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_after_preserving_uninstall");
  require(!response["configuration_preserved"].boolean(),"ssh_purge_confirmed_data_removal"); removed();
  invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_repeat_success");
  invoke({"/bin/bash","-s","--","uninstall"},script,true,"ssh_uninstall_repeat_success");
  seed_data();
  const auto staged = cache/"releases/0123456789abcdef0123456789abcdef/build/surfxr-client";
  atomic_write(staged,binary,0755);
  invoke({staged.string(),"--deploy","purge"},"{}",true,"staged_cli_purge_can_delete_its_own_cache"); removed();
  seed_data(); atomic_write(native,binary,0755);
  invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_uses_installed_native_owner"); removed();
  seed_data();
  // An older native CLI could remove itself then report a canonical-path error.
  atomic_write(native,"#!/bin/bash\nrm -rf -- \"$HOME/.local/share/surfxr-client\"\nprintf '{\"ok\":false,\"error\":\"frame_operation_failed\"}\\n'\nexit 1\n",0755);
  invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_completes_old_native_self_delete_failure"); removed();
  seed_data();
  atomic_write(native,"#!/bin/bash\nrm -rf -- \"$HOME/.local/share/surfxr-client\"\nprintf '{\"ok\":false,\"error\":\"frame_user_service_failed\"}\\n'\nexit 1\n",0755);
  invoke({"/bin/bash","-s","--","purge"},script,false,"ssh_preserves_known_native_failure", "frame_user_service_failed");
  require(fs::exists(config/"connection.json"),"known_native_failure_does_not_enter_residual_cleanup");
  invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_after_native_failure_reported"); removed();
  seed_data();
  fs::remove_all(config); fs::create_directory_symlink(external,config);
  invoke({"/bin/bash","-s","--","purge"},script,false,"ssh_purge_rejects_redirected_config", "purge_path_unverified");
  require(fs::exists(state/"settings.conf") && fs::exists(external/"kept"),"ssh_purge_preflight_before_deletion");
  fs::remove(config); atomic_write(config/"connection.json","fixture configuration");
  {
    Lock viewer(runtime/"viewer.lock");
    invoke({"/bin/bash","-s","--","purge"},script,false,"ssh_purge_refuses_live_viewer", "instance_active");
    require(fs::exists(config/"connection.json") && fs::exists(cache),"locked_residual_cleanup_keeps_data");
  }
  invoke({"/bin/bash","-s","--","purge"},script,true,"ssh_purge_after_lock_release"); removed();

  Paths paths{home,app,config/"connection.json",state,runtime,app/"bin/surfxr-viewer"};
  write_connection(paths,new_connection("direct"));
  const auto daemon = ::fork(); require(daemon >= 0,"install_fixture_daemon_fork");
  if (!daemon) {
    try { ::_exit(serve(paths,[] { return parse_network(Json::array(),Json::array(),Json::array(),""); })); }
    catch (...) { ::_exit(1); }
  }
  struct Child {
    pid_t pid;
    ~Child() { ::kill(pid,SIGTERM); while (::waitpid(pid,nullptr,0) < 0 && errno == EINTR) {} }
  } child{daemon};
  bool ready = false;
  const auto deadline = Clock::now()+std::chrono::seconds(3);
  do {
    try { ready = ipc(paths,object({{"command","status"}}))["ok"].boolean(); } catch (...) {}
    if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (!ready && Clock::now() < deadline);
  require(ready,"install_fixture_connection_service_ready");
  Fd overlay(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  const auto endpoint = (runtime/"overlay.sock").string();
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);
  require(overlay.get() >= 0 && !::bind(overlay.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)) &&
          !::chmod(endpoint.c_str(),0600),"install_fixture_dashboard_socket_ready");
  const auto install_build = home/"install-build";
  const auto installer = install_build/"surfxr-client";
  atomic_write(installer,binary,0755);
  for (auto name : {"surfxr-dashboard","surfxr-viewer","surfxr-discover","surfxr-runtime-bridge"}) atomic_write(install_build/name,read_file("/bin/true",1024*1024),0755);
  if(driver_tool)atomic_write(install_build/"driver_00_surfxr_exo_input.so","optional system input bridge",0755);
  const std::string request = R"({"mode":"desktop","link":"direct"})";
  response = invoke({installer.string(),"--deploy","install"},request,true,"install_opens_dashboard_after_releasing_locks");
  char wakeup[16]{};
  require(response["installed"].boolean() && response["dashboard_started"].boolean() &&
          ::recv(overlay.get(),wakeup,sizeof(wakeup),0) == 4 && std::string(wakeup,4) == "show",
          "install_uses_existing_dashboard_show_protocol");
  require(ipc(paths,object({{"command","status"}}))["pid"].is(json_type_null),"install_dashboard_does_not_start_streaming");
  require(sha256(app/"bin/surfxr-runtime-bridge")==sha256(install_build/"surfxr-runtime-bridge"),"install_stages_distinct_runtime_bridge");
  require(response["input_driver_registered"].boolean()==driver_tool &&
          response["steamvr_restart_required"].boolean()==driver_tool,"install_reports_driver_registration_and_restart");
  const auto driver=app/"drivers/00_surfxr_exo_input";
  const auto registered=[&] {
    const auto drivers=Json::parse(read_file(registry,65536))["external_drivers"];
    return drivers.size()==1&&drivers.at(0).text()==driver.string();
  };
  if(driver_tool){
    require(read_file(driver/"bin/linuxarm64/driver_00_surfxr_exo_input.so",1024)=="optional system input bridge","install_stages_system_input_driver");
    const auto manifest=Json::parse(read_file(driver/"driver.vrdrivermanifest",1024));
    require(manifest["name"].text()=="00_surfxr_exo_input"&&manifest["alwaysActivate"].boolean(),"installed_driver_manifest_matches_factory_binary");
    require(registered(),"fresh_install_registers_driver_in_isolated_registry");
    invoke({"/usr/bin/env","VR_PATHREG_OVERRIDE="+home.string(),installer.string(),"--deploy","install"},
        request,false,"driver_registration_failure_fails_install","frame_input_driver_registration_failed");
  }
  const auto installed_service=read_file(home/".config/systemd/user/surfxr-client.service",65536);
  require(installed_service.find("%h/.local/share/surfxr-client/launcher/surfxr-client")!=std::string::npos,"installed_service_uses_installed_native_launcher");
  require(read_connection(paths)["backend"].text()=="surfxr","install_selects_surfxr_backend");
  atomic_write(home/"release-manifest.json",R"({"install_profile":"surfxr"})");
  auto previous=read_connection(paths);auto last=previous["last_hosts"];last.set("direct","192.168.60.10");previous.set("last_hosts",last);write_connection(paths,previous);
  response=invoke({installer.string(),"--deploy","install"},request,true,"surfxr_upgrade_selects_backend");
  if(driver_tool)require(registered(),"upgrade_registration_is_idempotent");
  auto extended=read_connection(paths);
  require(response["backend"].text()=="surfxr"&&extended["backend"].text()=="surfxr"&&extended["surfxr"]["version"].text()=="21.0.0-dev14",
          "surfxr_install_selects_current_protocol");
  fs::remove(paths.config);
  response=invoke({installer.string(),"--deploy","install"},request,true,"surfxr_fresh_install_without_account");
  require(response["backend"].text()=="surfxr"&&read_connection(paths)["backend"].text()=="surfxr",
          "surfxr_fresh_install_backend_is_surfxr");
  response = invoke({"/usr/bin/env","SURFXR_LAUNCHER_FIXTURE_DASHBOARD_FAILURE=1",installer.string(),"--deploy","install"},
      request,true,"dashboard_start_failure_keeps_install_success");
  require(response["installed"].boolean() && !response["dashboard_started"].boolean() &&
          response["dashboard_error"].text() == "frame_dashboard_service_start_failed" &&
          fs::exists(native),"dashboard_start_failure_is_reported_without_undoing_install");
  if(driver_tool){
    invoke({native.string(),"--deploy","uninstall"},"{}",true,"uninstall_unregisters_driver");
    require(Json::parse(read_file(registry,65536))["external_drivers"].size()==0,"uninstall_clears_isolated_registration");
    invoke({installer.string(),"--deploy","install"},request,true,"reinstall_after_uninstall");
    require(registered(),"reinstall_restores_driver_registration");
    fs::remove(install_build/"driver_00_surfxr_exo_input.so");
    response=invoke({installer.string(),"--deploy","install"},request,true,"optional_driver_absent_install");
    require(!response["input_driver_registered"].boolean()&&!response["steamvr_restart_required"].boolean()&&registered(),
            "absent_payload_preserves_existing_driver_without_claiming_registration");
  }
}
}
