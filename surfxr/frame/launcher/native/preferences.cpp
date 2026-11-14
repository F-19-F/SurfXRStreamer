#include "controller.hpp"
#include "surfxr/settings_store.hpp"
#include "surfxr/image_quality.hpp"
#include "surfxr/input_preferences.hpp"
#include <cmath>

namespace surfxr::launcher {
namespace {
constexpr const char* groups[] = {"stream_settings", "image_quality", "input_preferences"};
constexpr const char* serial_names[] = {"setting", "quality", "input"};
constexpr const char* filenames[] = {"settings.conf", "image-quality.conf", "input-preferences.conf"};
const std::vector<std::string> names[] = {{"vr_fps", "vr_quality", "vr_bitrate_mbps", "foveated_streaming", "desktop_pointer_priority"},
  {"desktop_filter", "vr_upscaling", "vr_sharpening", "vr_sharpening_enabled", "vr_timewarp"},
  [] { std::vector<std::string> result; for (const auto& spec : kInputPreferenceSpecs) result.emplace_back(spec.name); return result; }()};
constexpr int stream_ids[] = {0,1,2,7,9};
std::string content(const Paths& p, int family) {
  auto path = p.state / filenames[family]; return fs::exists(path) ? read_file(path, family ? 1024 : 4096) : "";
}
Json values(const Paths& paths, int family) {
  auto data = Json::object(); const auto text = content(paths, family);
  if (family == 0) {
    surfxr::SettingsPreferences preferences;
    if (!text.empty() && !surfxr::parse_settings_preferences(text, preferences)) throw std::runtime_error("invalid_settings");
    for (size_t i = 0; i < names[0].size(); ++i) data.set(names[0][i], preferences.values[stream_ids[i]]);
  } else if (family == 1) {
    surfxr::ImageQualityPreferences preferences;
    if (!text.empty() && !surfxr::parse_image_quality(text, preferences)) throw std::runtime_error("invalid_image_quality");
    for (size_t i = 0; i < names[1].size(); ++i) data.set(names[1][i], surfxr::image_quality_value(preferences, static_cast<surfxr::ImageQualityId>(i)));
  } else {
    surfxr::InputPreferences preferences;
    if (!text.empty() && !surfxr::parse_input_preferences(text, preferences)) throw std::runtime_error("invalid_input_preferences");
    for (size_t i = 0; i < names[2].size(); ++i) data.set(names[2][i], surfxr::input_preference_value(preferences, static_cast<surfxr::InputPreferenceId>(i)));
  }
  return data;
}
void save(const Paths& paths, int family, int id, int value) {
  private_directory(paths.runtime); Lock lock(paths.runtime/"viewer.lock");
  const auto text = content(paths, family); std::string output;
  if (family == 0) {
    surfxr::SettingsPreferences preferences;
    if (!text.empty() && !surfxr::parse_settings_preferences(text, preferences)) throw std::runtime_error("invalid_settings");
    preferences.values[id] = value; if (!surfxr::valid_settings_preferences(preferences)) throw std::runtime_error("invalid_settings");
    output = surfxr::encode_settings_preferences(preferences);
  } else if (family == 1) {
    surfxr::ImageQualityPreferences preferences;
    if (!text.empty() && !surfxr::parse_image_quality(text, preferences)) throw std::runtime_error("invalid_image_quality");
    if (!surfxr::apply_image_quality_change(preferences, {static_cast<surfxr::ImageQualityId>(id), value})) throw std::runtime_error("invalid_quality");
    output = surfxr::encode_image_quality(preferences);
  } else {
    surfxr::InputPreferences preferences;
    if (!text.empty() && !surfxr::parse_input_preferences(text, preferences)) throw std::runtime_error("invalid_input_preferences");
    if (!surfxr::apply_input_preference_change(preferences, {static_cast<surfxr::InputPreferenceId>(id), value})) throw std::runtime_error("invalid_input");
    output = surfxr::encode_input_preferences(preferences);
  }
  private_directory(paths.state); atomic_write(paths.state/filenames[family], output);
}
}
Json Controller::preferences(int family, bool ok, const std::string& message) {
  auto response = result(ok, message); Json data; bool read_ok = true;
  try { data = values(paths_, family); }
  catch (...) { auto defaults = paths_; defaults.state = paths_.runtime/"missing-preferences"; data = values(defaults, family); read_ok = false; }
  for (const auto& name : names[family]) {
    const auto progress = progress_[family ? name : "requested_" + name];
    if (child_ && (progress.is(json_type_int) || progress.is(json_type_double) || (family && progress.is(json_type_boolean)))) {
      const auto value = progress.is(json_type_boolean) ? double(progress.boolean()) : progress.number();
      if (std::isfinite(value)) data.set(name, value);
    }
  }
  data.set("editable", read_ok && state_ != "stopping" && pending_[family].change.is(json_type_null));
  data.set("pending", pending_[family].change); data.set("message", message.empty() ? pending_[family].message : message);
  if (!read_ok) data.set("message", "Unable to read the stream settings.");
  if (family == 0) {
    // Retained local IPC key for existing Dashboard consumers.
    const bool extended=response["backend"].text()=="surfxr";data.set("evo_video_controls",extended);
    data.set("gaze_editable", data["editable"].boolean() && (response["backend"].text() != "surfxr" || !child_));
    if (response["backend"].text() == "surfxr") {
      data.set("editable", data["editable"].boolean() && child_ && progress_["max_vr_bitrate_mbps"].integer()==1000);
      if (read_ok && message.empty()) data.set("message", child_?"SurfXR: latest PC or headset edit wins. FPS / tier changes apply after you restart SteamVR.":"Connect to the PC to change SurfXR video settings. Gaze: next connection.");
    }
    const int limit = extended?1000:(!child_ || progress_["active_codec"].integer() == 5 ? 500 : 200);
    data.set("max_vr_quality", static_cast<int>(std::clamp<int64_t>(progress_["max_vr_quality"].integer(7), -1, 7)));
    data.set("max_vr_bitrate_mbps", static_cast<int>(std::clamp<int64_t>(progress_["max_vr_bitrate_mbps"].integer(limit), 10, limit)));
    auto rates = Json::array(); for (int rate : (extended?std::vector<int>{60,72,80,90,120,144}:surfxr::protocol_vr_framerates())) rates.push(rate); data.set("vr_framerates", rates);
    const bool gaze = progress_["eye_gaze_available"].boolean() || progress_["eye_gaze_available"].integer();
    data.set("eye_gaze_available", gaze);
    data.set("gaze_state", progress_["vr_foveated_streaming"].integer() || progress_["vr_foveated_streaming"].boolean() ? "active" :
      data["foveated_streaming"].number() == 0 ? "off" : child_ && !gaze ? "unavailable" : "waiting");
    for (auto key : {"vr_focus_width", "vr_focus_height", "vr_eye_width", "vr_eye_height", "vr_decoded_eye_width", "vr_decoded_eye_height"}) {
      const auto n = progress_[key].integer(); data.set(key, n >= 0 && n <= 16384 ? n : int64_t(0));
    }
    for (auto key : {"vr_focus_center_x", "vr_focus_center_y"}) {
      auto center = progress_[key]; bool valid = center.is(json_type_array) && center.size() == 2;
      for (size_t i = 0; i < center.size(); ++i) valid = valid && std::isfinite(center.at(i).number(-1)) && center.at(i).number(-1) >= 0 && center.at(i).number(-1) <= 1;
      if (!valid) { center = Json::array(); center.push(0); center.push(0); } data.set(key, center);
    }
  } else {
    for (size_t i = 0; i < names[family].size(); ++i) {
      const auto& key = names[family][i];
      const auto n = data[key].number();
      if ((family == 2 && kInputPreferenceSpecs[i].boolean) || key == "vr_upscaling" || key == "vr_sharpening_enabled" || key == "vr_timewarp") data.set(key, n != 0);
      else data.set(key, static_cast<int>(n));
    }
  }
  response.set(groups[family], data); return response;
}
Json Controller::change_preference(int family, const Json& request) {
  const auto name = request["setting"].text(); auto it = std::find(names[family].begin(), names[family].end(), name);
  if (it == names[family].end() || !request["value"].is(json_type_int)) return preferences(family, false, "Invalid request.");
  const auto n = request["value"].integer(); if (n < -1 || n > 1000) return preferences(family, false, "Invalid request.");
  const int value = static_cast<int>(n), index = static_cast<int>(it - names[family].begin()), id = family ? index : stream_ids[index];
  const auto current = preferences(family)[groups[family]];
  const bool saved_surfxr_gaze = family == 0 && name == "foveated_streaming" && current["gaze_editable"].boolean() && !child_ && result()["backend"].text() == "surfxr";
  if (!current["editable"].boolean() && !saved_surfxr_gaze) return preferences(family, false, current["message"].text("Wait for the current setting or disconnect operation to finish."));
  bool valid = false;
  if (family == 0) {
    const bool extended=current["evo_video_controls"].boolean();
    if(extended&&id>2&&id!=9&&!saved_surfxr_gaze)return preferences(family,false,"This control is configured on the PC.");
    const auto rates = extended?std::vector<int>{60,72,80,90,120,144}:surfxr::protocol_vr_framerates();
    valid = id == 0 ? std::find(rates.begin(), rates.end(), value) != rates.end() : id == 1 ? value >= -1 && value <= current["max_vr_quality"].integer() : id == 9 ? value == 0 || value == 1 : id == 7 ? value == 0 || value == 1 : value >= (extended?5:10) && value <= current["max_vr_bitrate_mbps"].integer();
  } else valid = family == 1 ? surfxr::valid_image_quality_change({static_cast<surfxr::ImageQualityId>(id), value}) : surfxr::valid_input_preference_change({static_cast<surfxr::InputPreferenceId>(id), value});
  if (!valid) return preferences(family, false, "Invalid request.");
  try {
    auto& pending = pending_[family];
    if (child_) {
      viewer_command(paths_, std::string(family == 0 ? "set" : family == 1 ? "quality" : "input") + " " + std::to_string(id) + " " + std::to_string(value) + "\n");
      pending.change = object({{"setting", name}, {"value", value}});
      pending.serial = progress_[std::string("last_") + serial_names[family] + "_request_serial"].integer();
      pending.deadline = Clock::now() + std::chrono::seconds(8); pending.message = "Applying the requested setting.";
    } else { save(paths_, family, id, value); pending.message = "Saved for the next connection."; }
  } catch (...) { return preferences(family, false, "Setting could not be saved or sent to the viewer."); }
  return preferences(family);
}
void Controller::poll_preferences() {
  for (int family = 0; family < 3; ++family) {
    auto& pending = pending_[family]; if (pending.change.is(json_type_null)) continue;
    const auto stem = std::string("last_") + serial_names[family] + "_request_";
    auto it = std::find(names[family].begin(), names[family].end(), pending.change["setting"].text());
    const int index = static_cast<int>(it - names[family].begin()), id = family ? index : stream_ids[index];
    if (progress_[stem+"serial"].integer() > pending.serial && progress_[stem+"id"].integer(-1) == id &&
        progress_[stem+"value"].number(-999) == pending.change["value"].number() && progress_[stem+"accepted"].is(json_type_boolean)) {
      pending.message = progress_[stem+"accepted"].boolean() ? "Setting accepted." : "The streaming process did not accept this setting."; pending.change = {};
    } else if (Clock::now() >= pending.deadline) { pending.message = "The setting was not confirmed. You can try again."; pending.change = {}; }
  }
}
}
