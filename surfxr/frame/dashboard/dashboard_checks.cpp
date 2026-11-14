#include "dashboard_runtime.hpp"

namespace surfxr::dashboard {
Status example(const std::string& state) {
  auto s = parse_status(R"({"ok":true,"state":"idle","configured":true,"connection_config":{
    "profile":"direct","profiles":{"auto":{"host":"192.168.9.119","local_address":""},
    "lan":{"host":"192.168.9.119","local_address":"192.168.9.163"},
    "direct":{"host":"10.35.78.27","local_address":"10.35.78.1"}},
    "interfaces":[{"name":"wlanap","address":"10.35.78.1","up":true}],
    "candidates":[{"host":"10.35.78.27","interface":"wlanap","kind":"direct","source":"saved"}],
    "effective":{"ready":true,"reason":"ready"}},
    "stream_settings":{"editable":true,"vr_quality":4,"vr_fps":120,"vr_bitrate_mbps":200,
    "max_vr_quality":7,"max_vr_bitrate_mbps":500,"vr_framerates":[24,30,60,72,80,90,96,100,120],
    "foveated_streaming":1,"eye_gaze_available":false,"gaze_state":"waiting",
    "message":"Saved for future connections."},
    "input_preferences":{"editable":true,"vr_controllers_enabled":true,"pending":null,"message":""},
    "image_quality":{"editable":true,"desktop_filter":0,"vr_upscaling":false,"vr_sharpening_enabled":false,
    "vr_sharpening":25,"vr_timewarp":true,"pending":null,"message":"Local processing on this Frame."}})");
  s.available = s.configured = s.ok = true;
  s.raw_battery_percent=96;s.raw_battery_charging=true; // Offline preview fixture.
  s.host = "10.35.78.27";
  s.state = state;
  s.mode = "vr";
  s.message = state == "streaming" ? "Streaming is active. You can disconnect here at any time." :
              state == "error" ? "No new desktop frame decoded within 10 seconds; source, keyframe and stream recovery exhausted." :
              "The PC Streamer is ready. Choose how you want to connect.";
  if (state == "error") {
    s.failure_code = "desktop_switch_timed_out"; s.failure_title = "Desktop switch timed out";
    s.failure_detail = s.message; s.failure_exit = 1;
    s.failure_action = "Choose Desktop to reconnect. If it repeats, save last-session.json and check PC desktop capture.";
  }
  return s;
}

void self_test() {
  auto require_line = [check = 0u](bool v,unsigned line) mutable {
    ++check;
    if (!v) throw std::runtime_error("Dashboard self-test failed at check " + std::to_string(check) + " (line " + std::to_string(line) + ").");
  };
#define require(...) require_line((__VA_ARGS__),__LINE__)
  struct KeyboardApi {
    uint64_t user_value = 0;
    unsigned hides = 0;
    std::string value = "192.168.50.10";
    bool allowed = true;
    bool empty_initial_text = false;
    vr::EVROverlayError ShowKeyboardForOverlay(vr::VROverlayHandle_t, vr::EGamepadTextInputMode,
        vr::EGamepadTextInputLineMode, uint32_t, const char*, uint32_t maximum, const char* initial, uint64_t token) {
      user_value = token;
      empty_initial_text = maximum == 15 && !*initial;
      return allowed ? vr::VROverlayError_None : vr::VROverlayError_RequestFailed;
    }
    uint32_t GetKeyboardText(char* output, uint32_t size) {
      const auto count = std::min(static_cast<size_t>(size - 1), value.size());
      std::memcpy(output, value.data(), count); output[count] = '\0';
      return static_cast<uint32_t>(value.size() + 1);
    }
    void HideKeyboard() { ++hides; }
  } keyboard_api;
  HostKeyboard<KeyboardApi> keyboard;
  vr::VREvent_t key_event{};
  std::string entered;
  require(keyboard.open(&keyboard_api, 1) && keyboard_api.empty_initial_text);
  key_event.eventType = vr::VREvent_KeyboardCharInput;
  key_event.data.keyboard.uUserValue = keyboard_api.user_value;
  require(!keyboard.accept(&keyboard_api, key_event, entered) && entered.empty());
  key_event.eventType = vr::VREvent_KeyboardDone;
  require(keyboard.accept(&keyboard_api, key_event, entered) && entered == keyboard_api.value && keyboard_api.hides == 1);
  entered.clear();
  require(keyboard.open(&keyboard_api, 1));
  require(!keyboard.accept(&keyboard_api, key_event, entered) && entered.empty()); // Previous keyboard's late Done.
  key_event.data.keyboard.uUserValue = keyboard_api.user_value;
  key_event.eventType = vr::VREvent_KeyboardClosed;
  require(!keyboard.accept(&keyboard_api, key_event, entered) && entered.empty());
  require(keyboard.open(&keyboard_api, 1));
  keyboard.cancel(&keyboard_api);
  key_event.data.keyboard.uUserValue = keyboard_api.user_value;
  key_event.eventType = vr::VREvent_KeyboardDone;
  require(!keyboard.accept(&keyboard_api, key_event, entered));
  keyboard_api.allowed = false;
  require(!keyboard.open(&keyboard_api, 1));
  keyboard_api.allowed = true;
  keyboard_api.value.assign(16, 'x');
  require(keyboard.open(&keyboard_api, 1));
  key_event.data.keyboard.uUserValue = keyboard_api.user_value;
  require(keyboard.accept(&keyboard_api, key_event, entered) && entered.empty());
  for(auto category:{SettingsPage::Stream,SettingsPage::Image,SettingsPage::Devices,SettingsPage::Diagnostics}){
    for(auto diagnostics:{DiagnosticsPage::Overlays,DiagnosticsPage::Content,DiagnosticsPage::Layout,DiagnosticsPage::Gaze}){
    auto page=example("idle");page.settings_page=category;page.diagnostics_page=diagnostics;
    require(settings_rows(category,diagnostics).size()<=4);
    for(int i=0;i<static_cast<int>(buttons.size());++i){
      if(!visible(page,i))continue;
      const auto b=button_rect(i);
      require(b.x>=0&&b.y>=0&&b.x+b.w<=width&&b.y+b.h<=height);
      require(hit(page,b.x+1,b.y+1)==i&&hit(page,b.x+b.w-1,b.y+b.h-1)==i);
      require(hit(page,b.x+b.w,b.y)==-1);
    }
    for(int i:{18,19,20,21,22,23,24,25,30,32,33})require(!visible(page,i));
    require(hit(page,0,0)==-1&&hit(page,std::numeric_limits<double>::quiet_NaN(),450)==-1);
    }
  }
  const Status s = parse_status("{\"ok\":true,\"state\":\"idle\",\"configured\":true,\"host\":\"pc\\\"name\",\"mode\":\"desktop\",\"pid\":null}\n");
  require(s.can_start() && !s.can_stop() && s.host == "pc\"name");
  const auto failure = parse_status(R"({"ok":true,"state":"error","configured":true,"failure":{
    "code":"desktop_switch_timed_out","title":"Desktop switch timed out","detail":"No new desktop frame in 10 seconds.",
    "action":"Choose Desktop to reconnect.","exit_code":1}})");
  require(failure.failure_code == "desktop_switch_timed_out" && failure.failure_exit == 1 &&
          failure.failure_action == "Choose Desktop to reconnect." && failure.can_start());
  require(merge_status(parse_status(R"({"ok":true,"state":"idle","configured":true,"failure":null})"),failure).failure_code.empty());
  require(!example("starting").can_start() && example("starting").can_stop());
  require(!example("streaming").can_start() && example("streaming").can_stop());
  require(!example("stopping").can_start() && !example("stopping").can_stop());
  require(example("error").can_start());
  auto switching = example("streaming");
  require(enabled(switching, 0) && !enabled(switching, 1) && enabled(switching, 2));
  switching.pending_mode = "desktop";
  require(!enabled(switching, 0) && enabled(switching, 1) && enabled(switching, 2));
  switching.state = "stopping";
  require(!enabled(switching, 0) && !enabled(switching, 1) && !enabled(switching, 2));
  const auto pending = parse_status("{\"ok\":true,\"state\":\"streaming\",\"configured\":true,\"mode\":\"desktop\",\"pending_mode\":\"vr\"}\n");
  require(pending.mode == "desktop" && pending.pending_mode == "vr" && enabled(pending, 0) && !enabled(pending, 1));
  const auto startup_json=[](const char* state,const char* reason){return std::string("{\"ok\":true,\"configured\":true,\"backend\":\"surfxr\",\"state\":\"streaming\",\"mode\":\"desktop\",\"steamvr_startup\":{\"state\":\"")+state+"\",\"reason\":\""+reason+"\",\"request_id\":17}}";};
  const auto launching=parse_status(startup_json("launching","none")),ready=parse_status(startup_json("runtime_ready","none"));
  const auto failed_startup=parse_status(startup_json("failed","runtime_exited"));
  require(launching.steamvr_startup_state=="launching"&&ready.steamvr_startup_state=="runtime_ready"&&
          !(launching==ready)&&failed_startup.steamvr_startup_request_id==17&&enabled(failed_startup,1));
  require(parse_status(startup_json("private_log_should_be_removed","none")).steamvr_startup_state.empty()&&
          parse_status(startup_json("failed","private_log_should_be_removed")).steamvr_startup_reason.empty());
  require(std::string(surfxr::steamvr_startup_detail(surfxr::SteamVrStartupState::Failed,surfxr::SteamVrStartupReason::RuntimeExited)).find("exited")!=std::string::npos);
  auto configured = example("idle");
  require(configured.connection.loaded && configured.connection.ready && configured.connection.profile == "direct");
  require(s.healthy_links == -1);
  const auto hosts = host_choices(configured.connection);
  require(hosts.size() == 2 && hosts.front().host.empty() && hosts[1].host == "10.35.78.27" && hosts[1].local_address == "10.35.78.1");
  require(enabled(configured, 3) && enabled(configured, 4) && !enabled(configured, 5) && enabled(configured, 6));
  require(enabled(configured,26));
  require(configured.connection.remember_last_ip && enabled(configured,27));
  require(enabled(configured,28) && activate("", configured,28).connection.signature == configured.connection.signature);
  const auto deferred = parse_status(R"({"ok":true,"state":"idle","configured":false,
      "connection_config":{"profile":"direct","profiles":{},
      "effective":{"ready":false,"reason":"discovery_required"}}})");
  require(!deferred.can_start() && enabled(deferred,26) && enabled(deferred,28));
  auto remember_off=parse_status(R"({"ok":true,"state":"idle","configured":true,"connection_config":{
    "profile":"direct","remember_last_ip":false,"last_hosts":{"direct":"10.35.78.42"}}})");
  require(!remember_off.connection.remember_last_ip && remember_off.connection.last_hosts[2]=="10.35.78.42");
  auto searching=configured;searching.connection.discovering=true;require(!enabled(searching,26));
  require(configured.quality.loaded && configured.quality.values[0] == 4 && configured.quality.values[2] == 200);
  require(configured.quality.values[3]==1&&enabled(configured,14)&&!enabled(configured,15));
  auto gaze=parse_status(R"({"ok":true,"configured":true,"state":"streaming","stream_settings":{"editable":true,
    "vr_quality":4,"vr_fps":120,"vr_bitrate_mbps":200,"max_vr_quality":7,"max_vr_bitrate_mbps":500,
    "foveated_streaming":1,"eye_gaze_available":true,"gaze_state":"active",
    "vr_focus_width":768,"vr_focus_height":832,"vr_eye_width":3072,"vr_eye_height":3264,
    "vr_decoded_eye_width":1024,"vr_decoded_eye_height":1088,"vr_focus_center_x":[0.38,0.42],
    "vr_focus_center_y":[0.47,0.49]}})");
  require(gaze.quality.gaze_state=="active"&&gaze.quality.gaze_available&&gaze.quality.focus_width==768&&
          gaze.quality.focus_height==832&&gaze.quality.eye_width==3072&&gaze.quality.base_height==1088);
  require(gaze.quality.center_x[0]==.38&&gaze.quality.center_y[1]==.49);
  gaze.quality.gaze_available=false;gaze.quality.gaze_state="unavailable";gaze.quality.choices[3]={0};
  gaze.quality.values[3]=0;require(!enabled(gaze,15));
  require(enabled(configured, 12) && enabled(configured, 13) && !enabled(configured, 11));
  require(next_value(configured.quality, 2, true) == 210 && next_value(configured.quality, 1, false) == 100);
  configured.quality.choices[2].erase(std::remove_if(configured.quality.choices[2].begin(), configured.quality.choices[2].end(),
      [](double value) { return value > 200; }), configured.quality.choices[2].end());
  require(!enabled(configured, 13)); // A 200 Mbps codec limit cannot be raised by the UI.
  const auto merged = merge_status(pending, configured);
  require(merged.connection.signature == configured.connection.signature && merged.quality.values == configured.quality.values);
  require(configured.input_preferences.loaded && configured.input_preferences.vr_controllers_enabled);
  auto devices=activate("",configured,34);
  require(devices.settings_page==SettingsPage::Devices&&settings_rows(devices.settings_page)[1].id==3);
  require(enabled(devices,8)&&!enabled(devices,9));
  require(enabled(devices,10)&&!enabled(devices,11));
  require(!enabled(devices,12)&&enabled(devices,13));
  devices.input_preferences.sync_raw_battery=true;require(enabled(devices,12)&&!enabled(devices,13));
  devices.input_preferences.editable=false;for(int i=8;i<14;++i)require(!enabled(devices,i));
  require(visible(devices,14)&&!enabled(devices,14)&&!enabled(devices,15)&&!visible(devices,33));
  auto battery_status=configured;battery_status.raw_battery_percent=92;
  require(!(battery_status==configured)&&merge_status(pending,battery_status).raw_battery_percent==92);
  const auto raw_preference=parse_status(R"({"ok":true,"state":"idle","configured":true,"input_preferences":{"editable":true,"vr_controllers_enabled":true,"sync_raw_battery":true}})");
  require(raw_preference.input_preferences.loaded&&raw_preference.input_preferences.sync_raw_battery);
  auto local=activate("",configured,17);
  require(local.settings_page==SettingsPage::Image&&!enabled(local,17)&&enabled(local,16));
  require(settings_rows(local.settings_page).size()==3&&settings_rows(local.settings_page)[2].id==4);
  auto diagnostics=activate("",local,35);
  require(!enabled(diagnostics,8)&&enabled(diagnostics,9)&&visible(diagnostics,12)&&enabled(diagnostics,13));
  diagnostics.input_preferences.performance_overlay=true;require(enabled(diagnostics,8)&&!enabled(diagnostics,9));
  diagnostics.input_preferences.gaze_debug_overlay=true;require(enabled(diagnostics,10)&&!enabled(diagnostics,11));
  require(merge_status(pending,diagnostics).settings_page==SettingsPage::Diagnostics);
  require(settings_rows(SettingsPage::Diagnostics).size()==4&&enabled(diagnostics,14)&&!enabled(diagnostics,15));
  require(input_preference_payload(15,0).find("transition_hud_enabled")!=std::string::npos);
  auto content=activate("",diagnostics,37);require(content.diagnostics_page==DiagnosticsPage::Content&&!enabled(content,37));
  require(settings_rows(content.settings_page,content.diagnostics_page).size()==4&&enabled(content,8)&&!enabled(content,9));
  auto layout=activate("",content,38);require(layout.diagnostics_page==DiagnosticsPage::Layout&&enabled(layout,8)&&enabled(layout,9));
  require(next_input_value(layout,12,true)==85&&next_input_value(layout,13,false)==15&&next_input_value(layout,14,true)==105);
  require(merge_status(pending,layout).diagnostics_page==DiagnosticsPage::Layout);
  layout.input_preferences.mini_hud_x=100;layout.input_preferences.mini_hud_y=0;layout.input_preferences.mini_hud_scale=200;
  require(!enabled(layout,9)&&!enabled(layout,10)&&!enabled(layout,13));
  layout.input_preferences.mini_hud_scale=50;require(!enabled(layout,12)&&enabled(layout,13));
  auto gaze_controls=activate("",layout,39);require(gaze_controls.diagnostics_page==DiagnosticsPage::Gaze&&enabled(gaze_controls,9)&&enabled(gaze_controls,10));
  require(input_preference_payload(12,85).find("\"value\":85")!=std::string::npos&&input_preference_payload(14,125).find("mini_hud_scale")!=std::string::npos);
  for(unsigned invalid:{16u,256u,268u}){bool rejected=false;
    try{input_preference_payload(invalid,1);}catch(const std::runtime_error&){rejected=true;}require(rejected);}
  const auto mini_status=[](const std::string& fields){return parse_status("{\"ok\":true,\"state\":\"idle\",\"configured\":true,\"input_preferences\":{\"editable\":true,\"vr_controllers_enabled\":true,"+fields+"}}");};
  const auto mini=mini_status("\"mini_hud_enabled\":true,\"mini_hud_gaze_only\":true,\"mini_hud_x\":95,\"mini_hud_y\":10,\"mini_hud_scale\":125");
  require(mini.input_preferences.loaded&&mini.input_preferences.mini_hud_enabled&&mini.input_preferences.mini_hud_gaze_only&&mini.input_preferences.mini_hud_x==95&&mini.input_preferences.mini_hud_scale==125);
  require(!mini_status("\"mini_hud_x\":true").input_preferences.loaded&&!mini_status("\"mini_hud_enabled\":1").input_preferences.loaded);
  require(!mini_status("\"mini_hud_scale\":201").input_preferences.loaded&&!mini_status("\"mini_hud_y\":-1").input_preferences.loaded);
  require(configured.input_preferences.mini_hud_x==80&&configured.input_preferences.mini_hud_y==20&&configured.input_preferences.mini_hud_scale==100);
  auto surfxr=local;surfxr.backend=surfxr.connection.backend="surfxr";surfxr.connection.backend_supported=true;
  surfxr.quality.editable=false;surfxr.quality.gaze_editable=true;
  require(enabled(surfxr,0)&&enabled(surfxr,1)&&enabled(surfxr,26)&&enabled(surfxr,28)&&enabled(surfxr,31));
  surfxr.connection.surfxr_version="20.14.1";require(!enabled(surfxr,0)&&!enabled(surfxr,26));surfxr.connection.surfxr_version="21.0.0-dev14";
  require(enabled(surfxr,12));surfxr.settings_page=SettingsPage::Stream;
  require(!enabled(surfxr,8)&&!enabled(surfxr,13)&&!enabled(surfxr,30));
  surfxr.state="streaming";surfxr.mode="vr";require(enabled(surfxr,31)&&enabled(surfxr,2));
  auto synced=parse_status(R"({"ok":true,"state":"streaming","mode":"vr","configured":true,"backend":"surfxr","stream_settings":{"evo_video_controls":true,"editable":true,"vr_quality":4,"vr_fps":144,"vr_bitrate_mbps":800,"desktop_pointer_priority":1,"max_vr_quality":7,"max_vr_bitrate_mbps":1000,"vr_framerates":[60,72,80,90,120,144]}})");
  require(synced.quality.loaded&&synced.quality.editable&&synced.quality.values[1]==144&&synced.quality.values[2]==800);
  require(enabled(synced,8)&&!enabled(synced,30));
  synced.settings_page=SettingsPage::Devices;require(synced.quality.values[5]==1&&enabled(synced,14)&&!enabled(synced,15));
  require(std::string(choice_label(settings_rows(synced.settings_page)[3],false))=="VR hand");
  const auto parsed_surfxr=parse_status(R"({"ok":true,"state":"idle","configured":true,"backend":"surfxr","connection_config":{"backend":"surfxr","surfxr":{"version":"21.0.0-dev14"}}})");
  require(parsed_surfxr.backend=="surfxr"&&parsed_surfxr.connection.surfxr_version=="21.0.0-dev14"&&parsed_surfxr.connection.backend_supported);
  require(!enabled(local, 8) && enabled(local, 9));
  require(enabled(local,12)&&!enabled(local,13)&&!visible(local,14));
  auto no_warp=local;no_warp.image_quality.choices[4].clear();require(!enabled(no_warp,12));
  local.image_quality.values[1]=0;require(!enabled(local,10)&&enabled(local,11));
  local.image_quality.values[2]=1; // Reserved storage never creates a control.
  require(settings_rows(local.settings_page)[2].id==4&&local.image_quality.values[3]==25);
  require(merge_status(pending,local).settings_page==SettingsPage::Image);
  local.state="streaming";require(enabled(local,0)&&!enabled(local,1)&&enabled(local,2));
  const auto pc_page=activate("",local,16);require(pc_page.settings_page==SettingsPage::Stream&&enabled(pc_page,0));
  local.image_quality.editable=false;for(int i=8;i<14;++i)require(!enabled(local,i));
  configured.state="stopping";
  for(int i=3;i!=exit_button;++i)if(!is_page_button(i))require(!enabled(configured,i));
  for (const char* state : {"offline", "idle", "starting", "streaming", "stopping", "error"}) {
    auto snapshot = example(state);
    if (std::strcmp(state,"offline") == 0) snapshot.available = false;
    snapshot.pid = 4321;
    for (bool local_page : {false, true}) {
      snapshot.settings_page=local_page?SettingsPage::Image:SettingsPage::Devices;
      require(enabled(snapshot,exit_button));
      require(activate("",snapshot,exit_button) == snapshot && stop_requested == 1);
      stop_requested = 0;
    }
  }
  TexturePublication publication;
  require(publication.front == -1 && publication.candidate() == 0);
  require(publication.complete(vr::VROverlayError_RequestFailed) == UploadResult::retry && publication.front == -1);
  require(publication.complete(vr::VROverlayError_None) == UploadResult::published && publication.front == 0);
  for (auto transient : {vr::VROverlayError_RequestFailed, vr::VROverlayError_InvalidTexture, vr::VROverlayError_TimedOut}) {
    require(publication.complete(transient) == UploadResult::retry);
    require(publication.front == 0 && publication.candidate() == 1);
  }
  require(publication.complete(vr::VROverlayError_None) == UploadResult::published && publication.front == 1);
  require(publication.complete(vr::VROverlayError_InvalidHandle) == UploadResult::reconnect && publication.front == 1);
  require(publication.complete(vr::VROverlayError_UnknownOverlay) == UploadResult::reconnect && publication.front == 1);
  require(publication.complete(vr::VROverlayError_None) == UploadResult::published && publication.front == 0);
  auto unconfigured = example("idle"); unconfigured.configured = false;
  require(!unconfigured.can_start());
  unconfigured.state = "streaming";
  require(unconfigured.can_select("desktop") && !unconfigured.can_select("vr") && unconfigured.can_stop());
  for (const char* bad : {"{}", "{\"ok\":1}", "[]", "{\"ok\":true,\"state\":\"bad\",\"configured\":true}",
                          "{\"ok\":true,\"state\":\"idle\",\"configured\":true} garbage"}) {
    bool rejected = false;
    try { (void)parse_status(bad); } catch (const std::exception&) { rejected = true; }
    require(rejected);
  }
  Canvas canvas(width, height);
  for (const char* state : {"idle", "starting", "streaming", "stopping", "error"}) {
    render(canvas, example(state), 0, -1);
    require(canvas.pixels() != nullptr && cairo_status(canvas.context()) == CAIRO_STATUS_SUCCESS);
    auto image = example(state);
    for(auto page:{SettingsPage::Image,SettingsPage::Devices,SettingsPage::Diagnostics})for(int battery:{-1,0,12,30,100}){
      image.settings_page=page;image.raw_battery_percent=battery;
      for(auto diagnostics:{DiagnosticsPage::Overlays,DiagnosticsPage::Content,DiagnosticsPage::Layout,DiagnosticsPage::Gaze}){
        image.diagnostics_page=diagnostics;
        render(canvas,image,17,-1);require(cairo_status(canvas.context())==CAIRO_STATUS_SUCCESS);
      }
    }
    require(cairo_status(canvas.context()) == CAIRO_STATUS_SUCCESS);
  }
  std::cout << "{\"status\":\"self_test_passed\",\"network_used\":false,\"openxr_session_started\":false,\"dashboard_checks_passed\":true}\n";
}


#undef require
} // namespace surfxr::dashboard
