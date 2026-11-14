#include "dashboard_model.hpp"

namespace surfxr::dashboard {
ConnectionConfig parse_connection(json_object* object) {
  ConnectionConfig c;
  if (!object) return c;
  c.loaded = true;
  c.backend_supported=member(object,"backend",json_type_string)!=nullptr;
  c.backend = string_member(object,"backend"); if(c.backend.empty())c.backend="surfxr";
  if(c.backend!="surfxr")throw std::runtime_error("Unknown streaming backend.");
  const auto version=string_member(member(object,"surfxr",json_type_object),"version");
  if(!version.empty())c.surfxr_version=version;
  c.signature = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
  c.profile = profile_names[profile_index(string_member(object, "profile"))];
  if (auto* remember = member(object, "remember_last_ip", json_type_boolean))
    c.remember_last_ip = json_object_get_boolean(remember);
  auto* last = member(object, "last_hosts", json_type_object);
  for (size_t i = 0; i < c.last_hosts.size(); ++i) c.last_hosts[i] = string_member(last, profile_names[i]);
  auto* profiles = member(object, "profiles", json_type_object);
  for (size_t i = 0; i != c.profiles.size(); ++i) {
    auto* profile = member(profiles, profile_names[i], json_type_object);
    c.profiles[i] = {string_member(profile, "host"), string_member(profile, "local_address"), "", ""};
  }
  auto* effective = member(object, "effective", json_type_object);
  c.ready = json_object_get_boolean(member(effective, "ready", json_type_boolean));
  c.reason = string_member(effective, "reason");
  if (auto* discovery = member(object, "discovery", json_type_object)) {
    c.discovering = json_object_get_boolean(member(discovery, "running", json_type_boolean));
    c.discovery_message = string_member(discovery, "message");
  }
  auto* candidates = member(object, "candidates", json_type_array);
  auto* interfaces = member(object, "interfaces", json_type_array);
  if (candidates) for (size_t i = 0; i < std::min<size_t>(16, json_object_array_length(candidates)); ++i) {
    auto* candidate = json_object_array_get_idx(candidates, i);
    if (!json_object_is_type(candidate, json_type_object)) continue;
    ConnectionChoice choice{string_member(candidate, "host"), string_member(candidate, "local_address"), string_member(candidate, "interface"), string_member(candidate, "kind")};
    if (choice.host.empty()) continue;
    if (interfaces && choice.local_address.empty()) for (size_t j = 0; j < std::min<size_t>(16, json_object_array_length(interfaces)); ++j) {
      auto* iface = json_object_array_get_idx(interfaces, j);
      if (string_member(iface, "name") == choice.interface &&
          json_object_get_boolean(member(iface, "up", json_type_boolean))) {
        choice.local_address = string_member(iface, "address"); break;
      }
    }
    c.candidates.push_back(std::move(choice));
  }
  return c;
}

QualitySettings parse_quality(json_object* object) {
  QualitySettings q;
  if (!object) return q;
  const auto number = [object](const char* name, double& result) {
    auto* value = member(object, name, json_type_int);
    if (!value) value = member(object, name, json_type_double);
    if (!value) return false;
    result = json_object_get_double(value);
    return std::isfinite(result);
  };
  // Retained local IPC key; the displayed product name is SurfXR.
  const bool extended=json_object_get_boolean(member(object,"evo_video_controls",json_type_boolean));
  double max_quality = -2, max_bitrate = 0;
  if (!number("vr_quality", q.values[0]) || !number("vr_fps", q.values[1]) ||
      !number("vr_bitrate_mbps", q.values[2]) || !number("max_vr_quality", max_quality) ||
      !number("max_vr_bitrate_mbps", max_bitrate)) return q;
  if (q.values[0] < (extended?-2:-1) || q.values[0] > 7 || std::floor(q.values[0]) != q.values[0] ||
      q.values[1] < 24 || q.values[1] > (extended?144:120) || q.values[2] < (extended?0:10) || q.values[2] > (extended?1000:500) ||
      max_quality < -2 || max_quality > 7 || max_bitrate < 10 || max_bitrate > (extended?1000:500)) return q;
  q.loaded = true;
  q.signature = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
  q.editable = json_object_get_boolean(member(object, "editable", json_type_boolean));
  auto* gaze_editable=member(object,"gaze_editable",json_type_boolean);
  q.gaze_editable=gaze_editable?json_object_get_boolean(gaze_editable):q.editable;
  q.message = string_member(object, "message", 160);
  for (int value = -1; value <= static_cast<int>(max_quality); ++value) q.choices[0].push_back(value);
  auto* rates = member(object, "vr_framerates", json_type_array);
  if (rates) for (size_t i = 0; i < std::min<size_t>(16, json_object_array_length(rates)); ++i) {
    auto* value = json_object_array_get_idx(rates, i);
    if (!json_object_is_type(value, json_type_int)) continue;
    const int hz = json_object_get_int(value);
    if (hz == 24 || hz == 30 || hz == 60 || hz == 72 || hz == 80 || hz == 90 || hz == 96 || hz == 100 || hz == 120 || (extended&&hz==144))
      q.choices[1].push_back(hz);
  }
  std::sort(q.choices[1].begin(), q.choices[1].end());
  q.choices[1].erase(std::unique(q.choices[1].begin(), q.choices[1].end()), q.choices[1].end());
  if(extended)q.choices[2].push_back(5);
  for (int value = 10; value <= static_cast<int>(max_bitrate); value += 10) q.choices[2].push_back(value);
  if(q.values[4]<0||q.values[4]>2||std::floor(q.values[4])!=q.values[4]){q.loaded=false;return q;}
  q.choices[4]={};
  if(number("desktop_pointer_priority",q.values[5])&&(q.values[5]==0||q.values[5]==1))q.choices[5]={0,1};
  q.values[3]=1;number("foveated_streaming",q.values[3]);
  if(q.values[3]!=0&&q.values[3]!=1){q.loaded=false;return q;}
  q.gaze_available=json_object_get_boolean(member(object,"eye_gaze_available",json_type_boolean));
  q.gaze_state=string_member(object,"gaze_state",32);
  q.choices[3]=(q.gaze_available||q.gaze_state=="waiting")?std::vector<double>{0,1}:std::vector<double>{0};
  const auto dimension=[&](const char* name){double v=0;return number(name,v)&&v>=0&&v<=16384&&std::floor(v)==v?static_cast<unsigned>(v):0u;};
  q.focus_width=dimension("vr_focus_width");q.focus_height=dimension("vr_focus_height");
  q.eye_width=dimension("vr_eye_width");q.eye_height=dimension("vr_eye_height");
  q.base_width=dimension("vr_decoded_eye_width");q.base_height=dimension("vr_decoded_eye_height");
  for(unsigned eye=0;eye<2;++eye)for(unsigned axis=0;axis<2;++axis){
    auto* values=member(object,axis?"vr_focus_center_y":"vr_focus_center_x",json_type_array);
    if(values&&json_object_array_length(values)==2){auto* value=json_object_array_get_idx(values,eye);
      if(value&&(json_object_is_type(value,json_type_double)||json_object_is_type(value,json_type_int))){
        const auto v=json_object_get_double(value);if(std::isfinite(v)&&v>=0&&v<=1)(axis?q.center_y:q.center_x)[eye]=v;
      }
    }
  }
  return q;
}

QualitySettings parse_image_quality(json_object* object) {
  QualitySettings q;
  if (!object) return q;
  auto* filter = member(object, "desktop_filter", json_type_int);
  auto* upscale = member(object, "vr_upscaling", json_type_boolean);
  auto* sharpen = member(object, "vr_sharpening_enabled", json_type_boolean);
  auto* strength = member(object, "vr_sharpening", json_type_int);
  if (!filter || !upscale || !sharpen || !strength) return q;
  const int f = json_object_get_int(filter), s = json_object_get_int(strength);
  if (f < 0 || f > 1 || s < 0 || s > 100) return q;
  q.values = {double(f), double(json_object_get_boolean(upscale)), double(json_object_get_boolean(sharpen)), double(s)};
  q.choices[0] = q.choices[1] = q.choices[2] = {0, 1};
  for (int value = 0; value <= 100; value += 5) q.choices[3].push_back(value);
  auto* timewarp=member(object,"vr_timewarp",json_type_boolean);
  q.values[4]=timewarp?double(json_object_get_boolean(timewarp)):1;
  if(timewarp)q.choices[4]={0,1};
  q.loaded = true;
  q.editable = json_object_get_boolean(member(object, "editable", json_type_boolean));
  q.message = string_member(object, "message", 160);
  q.signature = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
  return q;
}

InputPreferencesPanel parse_input_preferences(json_object* object) {
  InputPreferencesPanel p;
  if (!object) return p;
  auto* enabled = member(object, "vr_controllers_enabled", json_type_boolean);
  auto* editable = member(object, "editable", json_type_boolean);
  if (!enabled || !editable) return p;
  p.loaded = true;
  for(size_t index=0;index<kInputPreferenceCount;++index){
    const auto id=static_cast<InputPreferenceId>(index);const auto& spec=*input_preference_spec(id);
    json_object* value=nullptr;
    if(!json_object_object_get_ex(object,spec.name,&value))continue;
    if(!value||!json_object_is_type(value,spec.boolean?json_type_boolean:json_type_int)){p.loaded=false;return p;}
    const auto number=spec.boolean?int64_t(json_object_get_boolean(value)):json_object_get_int64(value);
    if(number<spec.minimum||number>spec.maximum||!apply_input_preference_change(p,{id,static_cast<int>(number)})){
      p.loaded=false;return p;
    }
  }
  p.editable = json_object_get_boolean(editable);
  p.message = string_member(object, "message", 160);
  p.signature = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
  return p;
}

Status parse_status(const std::string& data) {
  if (data.size() > 8192) throw std::runtime_error("Status response is too large.");
  json_tokener* tok = json_tokener_new_ex(8);
  if (!tok) throw std::runtime_error("Cannot read service status.");
  json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
  json_object* root = json_tokener_parse_ex(tok, data.data(), static_cast<int>(data.size()));
  const bool valid = json_tokener_get_error(tok) == json_tokener_success &&
                     root && json_object_is_type(root, json_type_object);
  json_tokener_free(tok);
  if (!valid) {
    if (root) json_object_put(root);
    throw std::runtime_error("The connection service returned invalid status.");
  }
  struct JsonGuard { json_object* p; ~JsonGuard() { json_object_put(p); } } guard{root};
  auto field = [root](const char* key, json_type type, bool required) {
    json_object* value = nullptr;
    if (json_object_object_get_ex(root, key, &value) && value && json_object_is_type(value, type))
      return value;
    if (required) throw std::runtime_error("The connection service status is incomplete.");
    return static_cast<json_object*>(nullptr);
  };
  Status s;
  s.available = true;
  s.ok = json_object_get_boolean(field("ok", json_type_boolean, true));
  s.configured = json_object_get_boolean(field("configured", json_type_boolean, true));
  s.state = json_object_get_string(field("state", json_type_string, true));
  if (s.state != "idle" && s.state != "starting" && s.state != "streaming" &&
      s.state != "stopping" && s.state != "error")
    throw std::runtime_error("The connection service returned an unknown state.");
  s.message = display_text(json_object_get_string(field("message", json_type_string, false)), 180);
  if (auto* failure = field("failure",json_type_object,false)) {
    s.failure_code = string_member(failure,"code",64);
    s.failure_title = string_member(failure,"title",80);
    s.failure_detail = string_member(failure,"detail",180);
    s.failure_action = string_member(failure,"action",180);
    s.failure_exit = json_object_get_int64(member(failure,"exit_code",json_type_int));
  }
  s.host = display_text(json_object_get_string(field("host", json_type_string, false)), 96);
  if(auto* backend=field("backend",json_type_string,false))s.backend=json_object_get_string(backend);
  if(s.backend!="surfxr")throw std::runtime_error("Unknown streaming backend.");
  auto* mode = field("mode", json_type_string, false);
  s.mode = mode && std::strcmp(json_object_get_string(mode), "vr") == 0 ? "vr" : "desktop";
  if (auto* pending = field("pending_mode", json_type_string, false)) {
    const std::string value = json_object_get_string(pending);
    if (value == "desktop" || value == "vr") s.pending_mode = value;
  }
  if(auto* startup=field("steamvr_startup",json_type_object,false)){
    const auto state=surfxr::steamvr_startup_state(string_member(startup,"state",32));
    const auto reason=surfxr::steamvr_startup_reason(string_member(startup,"reason",32));
    if(state&&reason){
      s.steamvr_startup_state=surfxr::steamvr_startup_name(*state);s.steamvr_startup_reason=surfxr::steamvr_startup_reason_name(*reason);
      if(auto* serial=member(startup,"request_id",json_type_int);serial&&json_object_get_int64(serial)>=0)
        s.steamvr_startup_request_id=static_cast<std::uint64_t>(json_object_get_int64(serial));
    }
  }
  if (auto* pid = field("pid", json_type_int, false)) s.pid = json_object_get_int64(pid);
  s.connection = parse_connection(field("connection_config", json_type_object, false));
  s.quality = parse_quality(field("stream_settings", json_type_object, false));
  s.image_quality = parse_image_quality(field("image_quality", json_type_object, false));
  s.input_preferences = parse_input_preferences(field("input_preferences", json_type_object, false));
  return s;
}


} // namespace surfxr::dashboard
