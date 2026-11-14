#include "native_fixture.hpp"
#include "session_failure.hpp"
#include "surfxr/input_preferences.hpp"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace surfxr::launcher_test {
using namespace surfxr::launcher;
namespace {
volatile std::sig_atomic_t done = 0;
void signal_done(int) { done = 1; }
void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
template<class Predicate> void await(Controller& controller, Predicate predicate, const char* label,
                                   std::chrono::seconds timeout = std::chrono::seconds(4)) {
  const auto end = Clock::now()+timeout;
  do { controller.poll(); if (predicate()) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } while (Clock::now() < end);
  throw std::runtime_error(label);
}
}
int child_fixture(int argc, char** argv) {
  const auto name = executable().filename().string();
  if (name == "systemctl") { // Never contact the user's service manager.
    if (argc == 4 && std::string(argv[2]) == "start" && std::string(argv[3]) == "surfxr-dashboard.service") {
      if (std::getenv("SURFXR_LAUNCHER_FIXTURE_DASHBOARD_FAILURE")) return 1;
      try { // A real dashboard could exit immediately if install retained these locks.
        const auto override = std::getenv("SURFXR_LAUNCHER_FIXTURE_RUNTIME");
        const auto runtime = override ? fs::path(override) : Paths::user().runtime;
        Lock viewer(runtime/"viewer.lock"), overlay(runtime/"overlay.lock");
        if(override)atomic_write(runtime/"dashboard-started","started");
      } catch (...) { return 1; }
    }
    return 0;
  }
  if (name != "surfxr-viewer" && name != "surfxr-discover") return -999;
  bool surfxr=false,start_vr=false,surfxr_desktop=false;std::string version,width,height,control,desktop_port;
  for(int i=1;i<argc;++i){const std::string key=argv[i];if(key=="--start-vr")start_vr=true;if(key=="--surfxr-desktop")surfxr_desktop=true;
    if(i+1<argc){if(key=="--backend")surfxr=std::string(argv[i+1])=="surfxr";
      if(key=="--surfxr-version")version=argv[i+1];
      if(key=="--surfxr-view-width")width=argv[i+1];
      if(key=="--surfxr-view-height")height=argv[i+1];
      if(key=="--surfxr-control-port")control=argv[i+1];
      if(key=="--surfxr-desktop-port")desktop_port=argv[i+1];}}
  std::string identity; std::getline(std::cin,identity);
  if(!identity.empty()||!surfxr)return 1;
  if(surfxr_desktop&&(version!="21.0.0-dev14"||desktop_port!="19950"))return 1;
  if(name=="surfxr-viewer"&&(start_vr||(version!="21.0.0-dev14"&&version!="20.14.1")||width!="3072"||height!="3264"||control!="19943"))return 1;
  std::string host, local, status; bool scan = false;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--scan") scan = true;
    if (i+1 < argc) { if (key == "--host") host = argv[i+1]; if (key == "--local-address") local = argv[i+1]; if (key == "--status-file") status = argv[i+1]; }
  }
  if (name == "surfxr-discover") {
    // Deliberately delayed completion exercises connection replacement/cancellation.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!scan && host == "192.168.50.99") return 1;
    auto device = object({{"status","discovered"},{"connection_started",false},{"source",host.empty() ? local.substr(0,local.rfind('.')+1)+"10" : host}});
    if(surfxr)device.set("backend","surfxr");
    if (scan) { auto devices = Json::array(); devices.push(device); device = object({{"status","scan_complete"},{"connection_started",false},{"devices",devices}}); }
    std::cout << device.dump(); return 0;
  }
  const fs::path runtime = fs::path(status).parent_path(); Lock lock(runtime/"viewer.lock");
  Fd socket(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
  sockaddr_un address{}; address.sun_family = AF_UNIX; const auto endpoint = (runtime/"viewer-command.sock").string();
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1); fs::remove(endpoint);
  if (::bind(socket.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))) return 1;
  done = 0; ::signal(SIGINT,signal_done); ::signal(SIGTERM,signal_done);
  auto progress = object({{"state","streaming"},{"mode",surfxr&&!surfxr_desktop?"vr":"desktop"},{"last_vr_requested",false},{"active_codec",2},{"requested_vr_quality",2},{"requested_vr_fps",120},{"requested_vr_bitrate_mbps",78},{"requested_foveated_streaming",1}});
  atomic_write(status,progress.dump());
  if(surfxr&&std::getenv("SURFXR_LAUNCHER_FIXTURE_REMOTE_STOP")){
    std::this_thread::sleep_for(std::chrono::milliseconds(500));fs::remove(endpoint);
    std::cout<<R"({"xr_failed":false,"receiver":{"backend":"surfxr","status":"server_stopped","server_stopped":true}})";return 0;
  }

  int64_t serial = 0,startup_serial=0;
  while (!done) {
    char buffer[128]; const auto count = ::recv(socket.get(),buffer,sizeof(buffer),0);
    if (count > 0) {
      const std::string command(buffer,static_cast<size_t>(count));
      if (command == "vr\n" || command == "desktop\n") {
        progress.set("last_vr_requested",command == "vr\n");
        if (true)
          progress.set("mode",command.substr(0,command.size()-1));
        if(surfxr&&std::getenv("SURFXR_LAUNCHER_FIXTURE_STARTUP")){
          progress.set("mode","desktop");
          progress.set("steamvr_startup",object({{"state",command=="vr\n"?"launching":"not_started"},{"reason","none"},{"request_id",++startup_serial}}));
        }
      }
      else {
        // Request receipt and acceptance are separate fixture transitions.
        int id = -1, value = -1; char kind[16]{};
        if (::sscanf(command.c_str(),"%15s %d %d",kind,&id,&value) == 3) {
          const std::string family = std::string(kind) == "set" ? "setting" : std::string(kind) == "quality" ? "quality" : "input";
          const auto stem = "last_"+family+"_request_";
          progress.set(stem+"serial",++serial); progress.set(stem+"id",id); progress.set(stem+"value",value);
          progress.set(stem+"accepted",Json{});
          atomic_write(status,progress.dump()); std::this_thread::sleep_for(std::chrono::milliseconds(60));
          if(family=="input"&&id>=0&&static_cast<unsigned>(id)<kInputPreferenceCount){
            const auto& spec=kInputPreferenceSpecs[static_cast<unsigned>(id)];
            progress.set(spec.name,spec.boolean?Json(value!=0):Json(value));
          }
          progress.set(stem+"accepted",true);
        }
      }
      atomic_write(status,progress.dump());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  fs::remove(endpoint);

  std::cout << "{\"xr_failed\":false,\"numeric_fixture\":123,\"private_fixture\":\"should_be_removed\"}"; return 0;
}

void ipc_checks(Paths paths, const Json& network) {
  const auto pid = ::fork(); require(pid >= 0,"ipc_fixture_fork");
  if (!pid) { try { ::_exit(serve(paths,[network] { return network; })); } catch (...) { ::_exit(1); } }
  struct Child {
    pid_t pid;
    ~Child() { ::kill(pid,SIGTERM); while (::waitpid(pid,nullptr,0) < 0 && errno == EINTR) {} }
  } child{pid};
  Json response;
  const auto deadline = Clock::now()+std::chrono::seconds(3);
  while (Clock::now() < deadline) {
    try { response = ipc(paths,object({{"command","config_get"}})); break; } catch (...) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(response["ok"].boolean(),"native_ipc_config_request");
  auto mini=ipc(paths,object({{"command","input_preferences_set"},{"setting","mini_hud_x"},{"value",37}}));
  require(mini["ok"].boolean()&&mini["input_preferences"]["mini_hud_x"].is(json_type_int)&&mini["input_preferences"]["mini_hud_x"].integer()==37,"mini_hud_ipc_numeric_response");
  mini=ipc(paths,object({{"command","input_preferences_get"}}));
  require(mini["input_preferences"]["mini_hud_x"].integer()==37,"mini_hud_ipc_persisted_round_trip");
  bool lock_rejected = false;
  try { Lock duplicate(paths.runtime/"service.lock"); } catch (...) { lock_rejected = true; }
  require(lock_rejected,"native_daemon_single_writer");
  sockaddr_un address{}; address.sun_family = AF_UNIX; const auto endpoint = (paths.runtime/"control.sock").string();
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);
  Fd bad(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
  require(!::connect(bad.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)),"bounded_request_connect");
  const auto junk = std::string(1024,'x'); require(::send(bad.get(),junk.data(),junk.size(),MSG_NOSIGNAL) == 1024,"bounded_request_sent");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  require(ipc(paths,object({{"command","status"}}))["ok"].boolean(),"oversized_request_does_not_stop_daemon");
}
void surfxr_checks(Paths paths,const Json& network) {
  paths.viewer=paths.home/"fixture-native/surfxr-viewer";
  const auto binaries=paths.viewer.parent_path();fs::create_directories(binaries);
  for(const auto* name:{"surfxr-viewer","surfxr-discover","systemctl"}){fs::copy_file(executable(),binaries/name);::chmod((binaries/name).c_str(),0755);}
  const auto old_path=std::getenv("PATH");
  struct FixtureEnvironment {
    bool present;std::string value;
    ~FixtureEnvironment(){if(present)::setenv("PATH",value.c_str(),1);else ::unsetenv("PATH");
      ::unsetenv("SURFXR_LAUNCHER_FIXTURE_RUNTIME");::unsetenv("SURFXR_LAUNCHER_FIXTURE_REMOTE_STOP");::unsetenv("SURFXR_LAUNCHER_FIXTURE_STARTUP");}
  } environment{old_path!=nullptr,old_path?old_path:""};
  ::setenv("PATH",(binaries.string()+":"+environment.value).c_str(),1);
  ::setenv("SURFXR_LAUNCHER_FIXTURE_RUNTIME",paths.runtime.c_str(),1);
  auto data=new_connection("direct");data.set("backend","surfxr");write_connection(paths,data);
  Controller controller(paths,[network]{return network;});
  require(controller.request(object({{"command","discover"}}))["ok"].boolean(),"surfxr_active_discovery_no_account");
  await(controller,[&]{return !controller.request(object({{"command","config_get"}}))["connection_config"]["discovery"]["running"].boolean();},"surfxr_find_both_interfaces");
  require(controller.request(object({{"command","config_get"}}))["connection_config"]["candidates"].size()==2,"surfxr_discovery_candidates_visible");
  require(controller.request(object({{"command","start"},{"mode","vr"}}))["ok"].boolean(),"surfxr_empty_host_starts_discovery");
  await(controller,[&]{return controller.result()["state"].text()=="streaming";},"surfxr_unique_link_auto_connects");
  controller.request(object({{"command","stop"}}));await(controller,[&]{return controller.result()["pid"].is(json_type_null);},"surfxr_discovered_session_stopped");
  require(controller.request(object({{"command","config_set"},{"profile","direct"},{"host","192.168.60.10"}}))["ok"].boolean(),"surfxr_save_manual_ip");
  require(controller.result()["configured"].boolean(),"surfxr_does_not_require_external_identity");
  require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean(),"surfxr_desktop_uses_dashboard_trust_without_key");
  await(controller,[&]{return controller.result()["state"].text()=="streaming";},"surfxr_desktop_no_key_connects");
  controller.request(object({{"command","stop"}}));await(controller,[&]{return controller.result()["pid"].is(json_type_null);},"surfxr_desktop_no_key_disconnects");
  require(!controller.request(object({{"command","settings_set"},{"setting","vr_fps"},{"value",90}}))["ok"].boolean(),"surfxr_video_settings_belong_to_pc");
  require(controller.request(object({{"command","settings_set"},{"setting","foveated_streaming"},{"value",0}}))["ok"].boolean(),"surfxr_gaze_saved_for_next_connection");
  for(auto version:{"21.0.0-dev14","20.14.1"}) {
    require(controller.request(object({{"command","config_set"},{"profile","direct"},{"surfxr",object({{"version",version}})}}))["ok"].boolean(),"historical_request_upgrades_to_current_protocol");
    require(controller.request(object({{"command","start"},{"mode","vr"}}))["ok"].boolean(),"surfxr_native_start");
    await(controller,[&]{return controller.result()["state"].text()=="streaming";},"surfxr_exact_cli_no_account_stdin");
    const auto pid=controller.result()["pid"].integer();
    require(controller.result()["backend"].text()=="surfxr","active_backend_status");
    require(!controller.request(object({{"command","config_set"},{"profile","direct"},{"backend","unsupported"}}))["ok"].boolean(),"active_backend_change_rejected");
    require(read_connection(paths)["surfxr"]["version"].text()=="21.0.0-dev14","only_current_protocol_is_persisted");
    {
      require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean(),"surfxr_v21_desktop_switch_request");
      await(controller,[&]{return controller.result()["mode"].text()=="desktop";},"surfxr_v21_desktop_switch_completion");
      require(controller.result()["pid"].integer()==pid,"surfxr_v21_mode_keeps_viewer_pid");
    }
    require(!controller.request(object({{"command","settings_set"},{"setting","foveated_streaming"},{"value",1}}))["ok"].boolean(),"surfxr_atlas_not_changed_during_session");
    auto hud=controller.request(object({{"command","input_preferences_set"},{"setting","performance_overlay"},{"value",1}}));
    require(!hud["input_preferences"]["pending"].is(json_type_null),"surfxr_hud_waits_for_acceptance");
    await(controller,[&]{return controller.request(object({{"command","input_preferences_get"}}))["input_preferences"]["pending"].is(json_type_null);},"surfxr_hud_uses_existing_serial_contract");
    const auto raw=controller.request(object({{"command","input_preferences_set"},{"setting","sync_raw_battery"},{"value",1}}));
    require(!raw["input_preferences"]["pending"].is(json_type_null),"raw_battery_waits_for_acceptance");
    await(controller,[&]{return controller.request(object({{"command","input_preferences_get"}}))["input_preferences"]["pending"].is(json_type_null);},"raw_battery_uses_existing_serial_contract");
    for(const auto& change:std::array<std::pair<const char*,int>,3>{{{"mini_hud_enabled",1},{"mini_hud_x",23},{"mini_hud_scale",145}}}){
      const auto requested=controller.request(object({{"command","input_preferences_set"},{"setting",change.first},{"value",change.second}}));
      require(requested["ok"].boolean()&&!requested["input_preferences"]["pending"].is(json_type_null),"mini_hud_waits_for_acceptance");
      await(controller,[&]{return controller.request(object({{"command","input_preferences_get"}}))["input_preferences"]["pending"].is(json_type_null);},"mini_hud_existing_serial_contract");
      const auto accepted=controller.request(object({{"command","input_preferences_get"}}))["input_preferences"];
      require(accepted["message"].text()=="Setting accepted.","mini_hud_command_not_accepted");
      const auto value=accepted[change.first];
      require(std::string(change.first)=="mini_hud_enabled"?(value.is(json_type_boolean)&&value.boolean()):(value.is(json_type_int)&&value.integer()==change.second),"mini_hud_live_value_or_type_lost");
    }
    controller.request(object({{"command","stop"}}));await(controller,[&]{return controller.result()["pid"].is(json_type_null);},"surfxr_shutdown_reaps_child");
  }
  require(!fs::exists(paths.runtime/"dashboard-started"),"user_disconnect_does_not_reopen_dashboard");
  controller.request(object({{"command","config_set"},{"profile","direct"},{"surfxr",object({{"version","21.0.0-dev14"}})}}));
  atomic_write(paths.state/"surfxr-desktop.key",std::string(64,'1')+"\n");
  require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean(),"surfxr_desktop_start_authorized");
  await(controller,[&]{return controller.result()["state"].text()=="streaming"&&controller.result()["mode"].text()=="desktop";},"surfxr_desktop_client_arguments");
  const auto desktop_pid=controller.result()["pid"].integer();
  require(controller.request(object({{"command","start"},{"mode","vr"}}))["ok"].boolean(),"surfxr_desktop_switch_to_vr");
  await(controller,[&]{return controller.result()["state"].text()=="streaming"&&controller.result()["mode"].text()=="vr";},"surfxr_desktop_switch_completed_same_process");
  require(controller.result()["pid"].integer()==desktop_pid,"surfxr_desktop_to_vr_preserves_pid");
  const auto vr_pid=controller.result()["pid"].integer();::chmod((paths.state/"surfxr-desktop.key").c_str(),0644);
  require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean()&&controller.result()["pid"].integer()==vr_pid,"surfxr_legacy_key_does_not_gate_dashboard_desktop");
  ::chmod((paths.state/"surfxr-desktop.key").c_str(),0600);
  require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean(),"surfxr_vr_switch_to_desktop");
  await(controller,[&]{return controller.result()["state"].text()=="streaming"&&controller.result()["mode"].text()=="desktop";},"surfxr_vr_switch_completed_same_process");
  require(controller.result()["pid"].integer()==vr_pid,"surfxr_vr_to_desktop_preserves_pid");
  controller.request(object({{"command","stop"}}));await(controller,[&]{return controller.result()["pid"].is(json_type_null);},"surfxr_desktop_disconnect");
  ::setenv("SURFXR_LAUNCHER_FIXTURE_STARTUP","1",1);
  require(controller.request(object({{"command","start"},{"mode","desktop"}}))["ok"].boolean(),"startup_fixture_desktop");
  await(controller,[&]{return controller.result()["state"].text()=="streaming";},"startup_fixture_presenting");
  const auto startup_pid=controller.result()["pid"].integer();
  require(controller.request(object({{"command","start"},{"mode","vr"}}))["ok"].boolean(),"startup_fixture_vr_requested");
  await(controller,[&]{return controller.result()["steamvr_startup"]["state"].text()=="launching";},"pc_startup_phase_launching");
  const auto update=[&](const char* state,const char* reason,bool transition){
    auto progress=Json::parse(read_file(paths.runtime/"viewer-status.json",16384));
    progress.set("mode","desktop");progress.set("last_transition_pending",transition);
    progress.set("steamvr_startup",object({{"state",state},{"reason",reason},{"request_id",int64_t(17)},{"private_log","should_be_removed"}}));
    atomic_write(paths.runtime/"viewer-status.json",progress.dump());controller.poll();return controller.result();
  };
  auto status=update("runtime_ready","none",true);
  require(status["pending_mode"].text()=="vr"&&status["steamvr_startup"]["state"].text()=="runtime_ready"&&
          status["message"].text().find("first VR frame")!=std::string::npos,"pc_ready_is_not_presented_completion");
  status=update("failed","runtime_exited",true);
  require(status["pending_mode"].text()=="vr","startup_failure_keeps_inflight_transition");
  status=update("failed","runtime_exited",false);
  require(status["pending_mode"].is(json_type_null)&&status["mode"].text()=="desktop"&&status["pid"].integer()==startup_pid&&
          status["message"].text().find("exited")!=std::string::npos&&status.dump().find("should_be_removed")==std::string::npos,"startup_failure_fallback_is_explained_and_retryable");
  status=update("failed","private_log_should_be_removed",false);
  require(status["steamvr_startup"]["reason"].text()=="runtime_exited","startup_arbitrary_reason_not_forwarded");
  controller.request(object({{"command","start"},{"mode","vr"}}));
  await(controller,[&]{return controller.result()["steamvr_startup"]["state"].text()=="launching";},"startup_failed_vr_retry_same_child");
  controller.request(object({{"command","start"},{"mode","desktop"}}));
  await(controller,[&]{return controller.result()["pending_mode"].is(json_type_null)&&
      controller.result()["steamvr_startup"]["state"].text()=="not_started";},"startup_desktop_cancels_unpresented_vr");
  update("failed","transport_failed",false);
  controller.request(object({{"command","stop"}}));await(controller,[&]{return controller.result()["pid"].is(json_type_null);},"startup_fixture_stopped");
  require(controller.result()["steamvr_startup"]["reason"].text()=="transport_failed","startup_failure_retained_after_idle");
  ::unsetenv("SURFXR_LAUNCHER_FIXTURE_STARTUP");
  Fd overlay(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));sockaddr_un address{};
  address.sun_family=AF_UNIX;const auto endpoint=(paths.runtime/"overlay.sock").string();
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);fs::remove(endpoint);
  require(!::bind(overlay.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)),"remote_stop_overlay_fixture_bound");
  ::chmod(endpoint.c_str(),0600);
  ::setenv("SURFXR_LAUNCHER_FIXTURE_REMOTE_STOP","1",1);
  controller.request(object({{"command","config_set"},{"profile","direct"},{"surfxr",object({{"version","21.0.0-dev14"}})}}));
  require(controller.request(object({{"command","start"},{"mode","vr"}}))["ok"].boolean(),"remote_stop_fixture_started");
  await(controller,[&]{char message[16]{};const auto n=::recv(overlay.get(),message,sizeof(message),0);
    return n==4&&std::string(message,4)=="show";},"remote_stop_restores_existing_dashboard_after_viewer_reaped");
  require(controller.result()["state"].text()=="idle"&&controller.result()["message"].text()=="PC SteamVR exited. Ready to reconnect."&&
    fs::exists(paths.runtime/"dashboard-started"),"remote_stop_clean_exit_and_dashboard_locks_released");
  fs::remove(endpoint);
  ::unsetenv("SURFXR_LAUNCHER_FIXTURE_REMOTE_STOP");
  // Real deployment -> Unix IPC -> controller -> fixture viewer. A saved LAN
  // profile must not silently defeat an explicitly requested direct link.
  {
    auto selected=read_connection(paths);selected.set("profile","lan");auto endpoints=selected["profiles"];
    endpoints.set("lan",object({{"host","192.168.50.10"},{"local_address",""}}));
    endpoints.set("direct",object({{"host","192.168.60.10"},{"local_address",""}}));
    selected.set("profiles",endpoints);write_connection(paths,selected);
    const auto daemon=::fork();require(daemon>=0,"link_start_daemon_fork");
    if(!daemon){try{::_exit(serve(paths,[network]{return network;}));}catch(...){::_exit(1);}}
    struct Daemon {pid_t pid;~Daemon(){::kill(pid,SIGTERM);while(::waitpid(pid,nullptr,0)<0&&errno==EINTR){}}} daemon_owner{daemon};
    const auto deadline=Clock::now()+std::chrono::seconds(3);bool ready=false;
    do{try{ready=ipc(paths,object({{"command","status"}}))["ok"].boolean();}catch(...){}
      if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }while(!ready&&Clock::now()<deadline);require(ready,"link_start_daemon_ready");
    const auto started=deployment(paths,"start",object({{"mode","vr"},{"link","direct"}}));
    require(started["ok"].boolean()&&started["state"].text()=="streaming"&&started["host"].text()=="192.168.60.10",
            "deployment_start_applies_link_instead_of_saved_lan");
    const auto running=ipc(paths,object({{"command","config_get"}}))["connection_config"];
    require(running["profile"].text()=="direct"&&running["active"]["profile"].text()=="direct"&&
            running["active"]["local_address"].text()=="192.168.60.2","active_link_matches_requested_interface");
    require(read_connection(paths)["profiles"].dump()==endpoints.dump(),"link_start_preserves_both_endpoints");
    const auto again=ipc(paths,object({{"command","start"},{"mode","vr"},{"profile","direct"}}));
    require(again["ok"].boolean()&&again["pid"].integer()==started["pid"].integer(),"same_link_start_keeps_existing_session");
    const auto refused=ipc(paths,object({{"command","start"},{"mode","vr"},{"profile","lan"}}));
    require(!refused["ok"].boolean()&&read_connection(paths)["profile"].text()=="direct","different_active_link_is_rejected_without_config_change");
    require(!ipc(paths,object({{"command","start"},{"mode","invalid"},{"profile","lan"}}))["ok"].boolean(),"invalid_mode_cannot_change_link");
    ipc(paths,object({{"command","stop"}}));
    const auto stopped=Clock::now()+std::chrono::seconds(3);
    while(!ipc(paths,object({{"command","status"}}))["pid"].is(json_type_null)&&Clock::now()<stopped)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(ipc(paths,object({{"command","status"}}))["pid"].is(json_type_null),"link_start_fixture_stopped");
  }
}
}
