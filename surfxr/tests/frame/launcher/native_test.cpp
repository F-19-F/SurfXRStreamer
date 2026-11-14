#include "controller.hpp"
#include "surfxr/ports.hpp"
#include "native_fixture.hpp"
#include "session_failure.hpp"
#include "surfxr/settings_store.hpp"
#include "surfxr/input_preferences.hpp"
#include <iostream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace surfxr::launcher;
namespace {
void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
template<class Function> void rejects(Function fn, const char* name) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } check(rejected,name); }
Json fixture_network() {
  const auto addresses = Json::parse(R"([{"ifname":"wifi","flags":["UP"],"addr_info":[{"family":"inet","local":"192.168.50.2","prefixlen":24}]},{"ifname":"ap","flags":["UP"],"addr_info":[{"family":"inet","local":"192.168.60.2","prefixlen":24}]}])");
  return parse_network(addresses,Json::array(),Json::array(),"Interface ap\n type AP\n channel 5 (5975 MHz)\n");
}
}
int main(int argc, char** argv) {
  try {
    const int fixture = surfxr::launcher_test::child_fixture(argc,argv);
    if (fixture != -999) return fixture;
    char pattern[] = "/tmp/surfxr-native-offline-XXXXXX";
    auto* temporary = ::mkdtemp(pattern); check(temporary,"temporary_home");
    const fs::path home = temporary;
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{home};
    Paths p{home,home/".local/share/surfxr-client",home/".config/surfxr-client/connection.json",home/".local/state/surfxr-client",home/"runtime/surfxr-client",home/".local/share/surfxr-client/bin/surfxr-viewer"};
    private_directory(p.runtime);
    rejects([] { (void)Json::parse("{} invalid"); },"json_trailing_data");
    rejects([] { (void)ipv4(Json("127.0.0.1")); },"loopback_ip");
    check(surfxr::ports::migrate_service_desktop(9950)==19950&&surfxr::ports::migrate_service_desktop(23456)==23456,"persisted_service_default_migration");
    auto cfg = new_connection(); write_connection(p,cfg);
    check(cfg["surfxr"]["control_port"].integer()==19943&&cfg["surfxr"]["desktop_port"].integer()==19950,"isolated_port_defaults");
    auto legacy_ports=cfg;auto legacy_fields=legacy_ports["surfxr"];
    legacy_fields.set("control_port",9943);legacy_fields.set("desktop_port",9950);legacy_ports.set("surfxr",legacy_fields);
    check(normalize_connection(legacy_ports).dump()==cfg.dump(),"legacy_port_migration_keeps_other_connection_fields");
    write_connection(p,legacy_ports);check(read_connection(p).dump()==cfg.dump(),"migrated_ports_persist");
    auto custom_fields=legacy_fields;custom_fields.set("control_port",23456);custom_fields.set("desktop_port",23460);
    auto custom_ports=cfg;custom_ports.set("surfxr",custom_fields);
    check(normalize_connection(custom_ports).dump()==custom_ports.dump(),"custom_ports_preserved");
    check(read_connection(p)["backend"].text()=="surfxr","default_backend_is_surfxr");
    check(cfg["surfxr"]["view_width"].integer()==3072&&cfg["surfxr"]["view_height"].integer()==3264,"default_launcher_godlike_resolution");
    auto bad_backend=cfg;bad_backend.set("backend","unknown");rejects([&]{normalize_connection(bad_backend);},"unknown_backend_rejected");
    const auto surfxr_failure=session_failure(Json::parse(R"({"receiver":{"backend":"surfxr","stage":"surfxr_handshake","error":"surfxr_tcp_media_deprecated_select_udp"}})"),1);
    check(surfxr_failure["code"].text()=="surfxr_tcp_media_deprecated_select_udp"&&surfxr_failure["action"].text().find("UDP")!=std::string::npos,"surfxr_failure_udp_guidance");
    const auto unknown_surfxr=session_failure(Json::parse(R"({"receiver":{"backend":"surfxr","stage":"surfxr_streaming","error":"fixture-secret"}})"),1);
    check(unknown_surfxr["action"].text().find("SteamVR")!=std::string::npos&&unknown_surfxr.dump().find("fixture-secret")==std::string::npos,"surfxr_unknown_failure_keeps_private_error_hidden");
    const auto desktop_failure=session_failure(Json::parse(R"({"receiver":{"backend":"surfxr","stage":"surfxr_desktop","error":"surfxr_receive_failed"}})"),1);
    check(desktop_failure["code"].text()=="surfxr_receive_failed"&&desktop_failure["action"].text().find("SurfXRDesktop")!=std::string::npos&&desktop_failure["action"].text().find("SteamVR")==std::string::npos,"desktop_receive_failure_is_retained_without_vr_guidance");
    const auto unified_failure=session_failure(Json::parse(R"({"receiver":{"backend":"surfxr","stage":"surfxr_desktop","error":"surfxr_unified_video_queue_full"}})"),1);
    check(unified_failure["code"].text()=="surfxr_unified_video_queue_full","unified_failure_reason_not_retained");
    const auto private_desktop=session_failure(Json::parse(R"({"receiver":{"backend":"surfxr","stage":"surfxr_desktop","error":"fixture-secret"}})"),1);
    check(private_desktop.dump().find("fixture-secret")==std::string::npos&&private_desktop["detail"].text().find("surfxr_desktop")!=std::string::npos,"unknown_desktop_error_keeps_only_stage");
    auto historical=cfg;historical.set("surfxr",object({{"version","20.14.1"}}));
    check(normalize_connection(historical)["surfxr"]["version"].text()=="21.0.0-dev14","historical_revision_upgrades_to_the_only_product_protocol");
    auto bad_surfxr=cfg;bad_surfxr.set("surfxr",object({{"version","21.0.0-dev13"}}));rejects([&]{normalize_connection(bad_surfxr);},"unknown_surfxr_version_rejected");
    bad_surfxr.set("surfxr",object({{"view_width",4097}}));rejects([&]{normalize_connection(bad_surfxr);},"surfxr_viewer_extent_limit");
    ::chmod(p.config.c_str(),0644); rejects([&] { (void)read_connection(p); },"config_permissions"); ::chmod(p.config.c_str(),0600);
    const auto network = fixture_network(); check(network["interfaces"].size() == 2,"network_interfaces");
    check(scan_sources(network).size() == 2,"both_discovery_links");
    check(effective_connection("direct",object({{"host","192.168.50.10"},{"local_address",""}}),network)["reason"].text() == "route_unavailable","no_link_fallback");
    check(effective_connection("direct",object({{"host",""},{"local_address",""}}),network)["reason"].text() == "discovery_required","empty_ip_discovery");
    auto unnamed = parse_network(Json::parse(R"([{"ifname":"wifi","flags":["UP"],"addr_info":[{"family":"inet","local":"192.168.50.2","prefixlen":24}]}])"),Json::array(),Json::array(),"Interface wifi\n type managed\n Unnamed/non-netdev interface\n type AP\n channel 5 (5975 MHz)\n");
    check(unnamed["interfaces"].at(0)["kind"].text() == "lan","unnamed_radio_is_not_ap");
    Controller controller(p,[network] { return network; });
    const auto fresh_settings=controller.request(object({{"command","settings_get"}}))["stream_settings"];
    check(fresh_settings["vr_fps"].number()==120&&fresh_settings["vr_quality"].number()==4&&
      fresh_settings["vr_bitrate_mbps"].number()==100&&fresh_settings["foveated_streaming"].number()==1&&
      fresh_settings["desktop_pointer_priority"].number()==0,"fresh_launcher_matches_pc_stream_defaults");
    auto public_state = controller.request(object({{"command","config_get"}}));
    check(controller.request(object({{"command","config_set"},{"profile","lan"},{"host","192.168.50.10"},{"remember_last_ip",false}}))["ok"].boolean(),"manual_ip_and_memory_off");
    check(!read_connection(p)["remember_last_ip"].boolean(),"memory_flag_persisted");
    auto before = read_file(p.config,16384,true);
    check(!controller.request(object({{"command","config_set"},{"profile","lan"},{"host","127.0.0.1"}}))["ok"].boolean(),"invalid_ip_request");
    check(read_file(p.config,16384,true) == before,"invalid_config_is_atomic");
    check(!controller.request(object({{"command","settings_set"},{"setting","vr_bitrate_mbps"},{"value",200}}))["ok"].boolean(),"offline_surfxr_settings_require_pc");
    check(controller.request(object({{"command","image_quality_set"},{"setting","vr_sharpening"},{"value",35}}))["ok"].boolean(),"hidden_strength_preserved");
    check(controller.request(object({{"command","image_quality_get"}}))["image_quality"]["vr_timewarp"].boolean(),"timewarp_default_on");
    check(controller.request(object({{"command","image_quality_set"},{"setting","vr_timewarp"},{"value",0}}))["ok"].boolean(),"timewarp_off_accepted");
    check(!controller.request(object({{"command","image_quality_get"}}))["image_quality"]["vr_timewarp"].boolean(),"timewarp_saved_off");
    check(controller.request(object({{"command","image_quality_get"}}))["image_quality"]["vr_sharpening"].integer()==35,"timewarp_preserves_hidden_strength");
    check(controller.request(object({{"command","input_preferences_set"},{"setting","performance_overlay"},{"value",1}}))["ok"].boolean(),"input_hud_preferences");
    check(controller.request(object({{"command","input_preferences_set"},{"setting","gaze_debug_overlay"},{"value",1}}))["input_preferences"]["gaze_debug_overlay"].boolean(),"input_gaze_preferences");
    check(controller.request(object({{"command","input_preferences_get"}}))["input_preferences"]["boundary_sync_enabled"].boolean(),"boundary_default_on");
    check(!controller.request(object({{"command","input_preferences_get"}}))["input_preferences"]["sync_raw_battery"].boolean(),"runtime_battery_default");
    check(controller.request(object({{"command","input_preferences_set"},{"setting","sync_raw_battery"},{"value",1}}))["input_preferences"]["sync_raw_battery"].boolean(),"raw_battery_saved_on");
    check(!controller.request(object({{"command","input_preferences_set"},{"setting","boundary_sync_enabled"},{"value",0}}))["input_preferences"]["boundary_sync_enabled"].boolean(),"boundary_saved_off");
    const auto mini_defaults=controller.request(object({{"command","input_preferences_get"}}))["input_preferences"];
    check(!mini_defaults["mini_hud_enabled"].boolean()&&mini_defaults["mini_hud_fps"].boolean()&&mini_defaults["mini_hud_latency"].boolean()&&
      mini_defaults["mini_hud_gaze_marker"].boolean()&&mini_defaults["mini_hud_x"].integer()==80&&mini_defaults["mini_hud_y"].integer()==20&&
      mini_defaults["mini_hud_scale"].integer()==100,"mini_hud_launcher_defaults");
    for(unsigned id=5;id<surfxr::kInputPreferenceCount;++id){
      const auto& spec=surfxr::kInputPreferenceSpecs[id];
      const auto saved=controller.request(object({{"command","input_preferences_set"},{"setting",spec.name},{"value",spec.maximum}}));
      check(saved["ok"].boolean(),"mini_hud_offline_save");
      const auto value=saved["input_preferences"][spec.name];
      check(spec.boolean?(value.is(json_type_boolean)&&value.boolean()):(value.is(json_type_int)&&value.integer()==spec.maximum),"mini_hud_json_type_preserved");
      for(const auto invalid:{spec.minimum-1,spec.maximum+1})
        check(!controller.request(object({{"command","input_preferences_set"},{"setting",spec.name},{"value",invalid}}))["ok"].boolean(),"mini_hud_offline_range_rejected");
    }
    check(controller.request(object({{"command","input_preferences_set"},{"setting","mini_hud_scale"},{"value",135}}))["input_preferences"]["mini_hud_scale"].integer()==135,"mini_hud_scale_integer_round_trip");
    surfxr::InputPreferences mini_saved;
    check(surfxr::parse_input_preferences(read_file(p.state/"input-preferences.conf",1024),mini_saved)&&mini_saved.mini_hud_scale==135&&mini_saved.mini_hud_enabled,"mini_hud_offline_values_persisted");
    {
      Lock viewer(p.runtime/"viewer.lock");
      check(!controller.request(object({{"command","settings_set"},{"setting","vr_quality"},{"value",3}}))["ok"].boolean(),"manual_viewer_excludes_offline_write");
      check(!controller.request(object({{"command","start"}}))["ok"].boolean() && controller.result()["pid"].is(json_type_null),"manual_viewer_excludes_launch");
    }
    surfxr::launcher_test::ipc_checks(p,network);
    surfxr::launcher_test::surfxr_checks(p,network);
    auto build = home/"build"; fs::create_directory(build);
    constexpr size_t fixture_binary_limit = 1024 * 1024; // ARM64 coreutils can exceed 64 KiB.
    for (auto name : {"surfxr-client","surfxr-viewer","surfxr-dashboard","surfxr-discover","surfxr-runtime-bridge"}) atomic_write(build/name,read_file("/bin/true",fixture_binary_limit),0755);
    fs::create_directories(build/"runtime/lib"); fs::create_directories(build/"runtime/licenses");
    atomic_write(build/"runtime/lib/libavcodec.so.61","private media fixture");
    atomic_write(build/"runtime/licenses/FFmpeg.LICENSE","license fixture");
    install_runtime(p,build);
    check(read_file(p.app/"lib/libavcodec.so.61",128)=="private media fixture" &&
          read_file(p.app/"licenses/FFmpeg.LICENSE",128)=="license fixture","installed_private_media_and_license");
    check(read_file(p.home/".config/systemd/user/surfxr-client.service",4096).find("%h/.local/share/surfxr-client/launcher/surfxr-client") != std::string::npos,"installed_service_execstart");
    check(fs::is_regular_file(p.app/"launcher/surfxr-client") && read_file(p.app/"launcher/surfxr-client",fixture_binary_limit).substr(0,4) == std::string("\x7f" "ELF",4),"installed_native_entry");
#if defined(__aarch64__)
    auto files=Json::object();
    for(const auto& name : {"surfxr-client","surfxr-viewer","surfxr-dashboard","surfxr-discover","surfxr-runtime-bridge",
                            "runtime/lib/libavcodec.so.61","runtime/licenses/FFmpeg.LICENSE"})
      files.set(std::string("payload/build/")+name,sha256(build/name));
    auto manifest=object({{"format_version",2},{"architecture","aarch64"},{"frame_python_required",false},{"files",files}});
    atomic_write(home/"release-manifest.json",manifest.dump());
    check(verify_release(home)["verified"].boolean(),"release_verifies_private_media_payload");
    atomic_write(build/"runtime/lib/libavcodec.so.61","tampered");
    rejects([&]{verify_release(home);},"release_rejects_tampered_private_library");
    atomic_write(build/"runtime/lib/libavcodec.so.61","private media fixture");
#endif
    remove_runtime(p); check(fs::exists(p.config) && fs::exists(p.state/"settings.conf"),"uninstall_preserves_configuration");
    check(!fs::exists(p.app) && !fs::is_symlink(p.home/".local/bin/surfxr-client"),"uninstall_removes_owned_files");
    auto external = home/"external"; fs::create_directory(external); atomic_write(external/"kept","fixture"); fs::create_directory_symlink(external,p.app);
    rejects([&] { remove_runtime(p); },"uninstall_rejects_symlink_root"); check(fs::exists(external/"kept"),"unrelated_files_preserved");
    fs::remove(p.app); install_runtime(p,build);
    const char* original_cache = std::getenv("XDG_CACHE_HOME");
    struct CacheEnvironment {
      bool present; std::string value;
      ~CacheEnvironment() { if (present) ::setenv("XDG_CACHE_HOME",value.c_str(),1); else ::unsetenv("XDG_CACHE_HOME"); }
    } cache_environment{original_cache != nullptr, original_cache ? original_cache : ""};
    ::setenv("XDG_CACHE_HOME",(home/"custom-cache").c_str(),1);
    const auto saved_config = home/"saved-config";
    fs::rename(p.config.parent_path(),saved_config); fs::create_directory_symlink(external,p.config.parent_path());
    rejects([&] { remove_runtime(p,true); },"purge_rejects_redirected_config_before_deleting_app");
    check(fs::exists(p.app/"launcher/surfxr-client") && fs::exists(p.state/"settings.conf") && fs::exists(external/"kept"),"purge_rejection_keeps_install_and_data");
    fs::remove(p.config.parent_path()); fs::rename(saved_config,p.config.parent_path());
    atomic_write(home/".cache/surfxr-client/releases/fixture/build/log","old release");
    atomic_write(home/"custom-cache/surfxr-client/fixture","cache");
    atomic_write(home/"custom-cache/other-application/kept","unrelated cache");
    fs::create_directory_symlink(external,p.state/"external-link");
    atomic_write(p.runtime/"viewer-status.json","fixture state");
    struct stat lock_before{}, lock_after{};
    {
      Lock service(p.runtime/"service.lock"), viewer(p.runtime/"viewer.lock"), overlay(p.runtime/"overlay.lock");
      check(!::stat((p.runtime/"viewer.lock").c_str(),&lock_before),"purge_lock_before");
      remove_runtime(p,true);
      check(!::stat((p.runtime/"viewer.lock").c_str(),&lock_after) && lock_before.st_ino == lock_after.st_ino,"purge_preserves_stable_lock_inode");
    }
    check(!fs::exists(p.app) && !fs::exists(p.config.parent_path()) && !fs::exists(p.state) &&
          !fs::exists(home/".cache/surfxr-client") && !fs::exists(home/"custom-cache/surfxr-client") &&
          !fs::exists(p.runtime/"viewer-status.json"),"full_uninstall_removes_connection_preferences_layout_logs_and_cache");
    check(fs::exists(external/"kept") && fs::exists(home/"custom-cache/other-application/kept"),"purge_preserves_siblings_and_symlink_targets");
    remove_runtime(p,true); // Already absent user data remains a successful purge.
    surfxr::launcher_test::deployment_checks(home/"removal");
    std::cout << "Native launcher configuration, preferences, link selection and isolated CLI/SSH install/uninstall passed; no streaming, XR or real systemd calls.\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "native launcher check: " << error.what() << '\n'; return 1; }
}
