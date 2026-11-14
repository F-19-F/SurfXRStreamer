#include "controller.hpp"
#include "surfxr/steamvr_startup.hpp"
#include <sys/stat.h>
#include <unistd.h>

namespace surfxr::launcher {
Controller::Controller(Paths paths, std::function<Json()> reader) : paths_(std::move(paths)), network_reader_(std::move(reader)) {
  private_directory(paths_.runtime);
}
Controller::~Controller() { shutdown(); }
Json Controller::network() {
  if (network_.is(json_type_null) || Clock::now() >= network_deadline_) { network_ = network_reader_(); network_deadline_ = Clock::now() + std::chrono::seconds(5); }
  return network_;
}
void Controller::update_startup(const Json& progress){
  const auto sample=progress["heartbeat_monotonic_ms"];
  if(sample.is(json_type_int)&&sample.integer()>=0&&static_cast<std::uint64_t>(sample.integer())<startup_requested_ms_)return;
  const auto source=progress["steamvr_startup"];
  const auto state=surfxr::steamvr_startup_state(source["state"].text());
  const auto reason=surfxr::steamvr_startup_reason(source["reason"].text());
  if(!state||!reason)return;
  if(*state==surfxr::SteamVrStartupState::NotStarted&&steamvr_startup_["state"].text()=="failed"&&pending_mode_!="vr")return;
  steamvr_startup_=object({{"state",surfxr::steamvr_startup_name(*state)},{"reason",surfxr::steamvr_startup_reason_name(*reason)},
    {"request_id",source["request_id"].is(json_type_int)&&source["request_id"].integer()>=0?source["request_id"]:Json(int64_t(0))}});
}
Json Controller::result(bool ok, const std::string& message) {
  if (!child_) {
    try { auto data = read_connection(paths_); auto c = public_connection(data, network(), {}, found_);
      host_ = c["effective"]["host"].text();
      configured_ = (!host_.empty()&&c["effective"]["ready"].boolean())||
        (data["surfxr"]["version"].text()=="21.0.0-dev14"&&scan_sources(network(),data["profile"].text(),data["profiles"][data["profile"].text()]["local_address"].text()).size());
    } catch (...) { host_.clear(); configured_ = false; }
  }
  std::string backend = active_["backend"].text();
  if (backend.empty()) try { backend = read_connection(paths_)["backend"].text(); } catch (...) { backend = "surfxr"; }
  std::string status_message=!configured_&&state_=="idle"?"Connection setup is required.":message_;
  const auto startup_state=surfxr::steamvr_startup_state(steamvr_startup_["state"].text());
  const auto startup_reason=surfxr::steamvr_startup_reason(steamvr_startup_["reason"].text());
  if(backend=="surfxr"&&startup_state&&startup_reason&&state_!="stopping"&&
     (*startup_state==surfxr::SteamVrStartupState::Failed||pending_mode_=="vr"||(mode_=="vr"&&state_!="idle")))
    status_message=std::string(surfxr::steamvr_startup_title(*startup_state))+". "+surfxr::steamvr_startup_detail(*startup_state,*startup_reason);
  return object({{"ok", ok}, {"state", state_}, {"mode", mode_}, {"backend", backend}, {"host", host_}, {"configured", configured_},
    {"message", message.empty()?status_message:message},
    {"pid", child_ ? Json(static_cast<int>(child_->pid())) : Json()}, {"last_exit", last_exit_},
    {"failure", failure_},
    {"steamvr_startup",steamvr_startup_.is(json_type_object)?steamvr_startup_:object({{"state","not_started"},{"reason","none"},{"request_id",int64_t(0)}})},
    {"pending_mode", pending_mode_.empty() ? Json() : Json(pending_mode_)}});
}
Json Controller::connection_result(bool ok, const std::string& message) {
  Json data;
  try { data = read_connection(paths_); } catch (...) { data = new_connection("auto"); }
  auto response = result(ok, message);
  auto c = public_connection(data, network(), active_, found_);
  c.set("discovery", object({{"running", !searches_.empty()}, {"message", search_message_}}));
  response.set("connection_config", c); return response;
}
Json Controller::configure(const Json& request) {
  try {
    const auto profile = request["profile"].text(); if (!valid_profile(profile)) throw std::runtime_error("invalid_profile");
    auto data = read_connection(paths_), endpoints = data["profiles"], last = data["last_hosts"];
    if (request.has("backend")) {
      if (data["backend"].text() != request["backend"].text()) {
        if (child_) return connection_result(false, "Disconnect before changing streaming backend.");
        for (auto name : profiles) last.set(name, "");
      }
      data.set("backend", request["backend"]);
    }
    if (request.has("surfxr")) {
      auto settings = data["surfxr"], changes = request["surfxr"];
      if (!changes.only({"version", "view_width", "view_height", "control_port", "desktop_port"})) throw std::runtime_error("invalid_surfxr_config");
      for (const auto& key : changes.keys()) settings.set(key, changes[key]);
      if (child_ && settings.dump() != data["surfxr"].dump()) return connection_result(false, "Disconnect before changing the SURFXR protocol.");
      data.set("surfxr", settings);
    }
    if (request.has("profiles")) {
      const auto changes = request["profiles"]; if (!changes.only({"lan", "direct"})) throw std::runtime_error("invalid_profiles");
      for (const auto& name : changes.keys()) {
        auto fields = changes[name], item = endpoints[name];
        if (!fields.only({"host", "local_address"})) throw std::runtime_error("invalid_endpoint");
        for (const auto& key : fields.keys()) item.set(key, ipv4(fields[key], true));
        if (fields.has("host")) last.set(name, "");
        endpoints.set(name, item);
      }
    }
    auto selected = endpoints[profile];
    for (auto key : {"host", "local_address"}) if (request.has(key)) selected.set(key, ipv4(request[key], true));
    if (request.has("host")) last.set(profile, "");
    endpoints.set(profile, selected); data.set("profiles", endpoints); data.set("profile", profile); data.set("last_hosts", last);
    if (request.has("remember_last_ip")) data.set("remember_last_ip", request["remember_last_ip"]);
    write_connection(paths_, data); cancel_search();  found_ = Json::array();
  } catch (...) { return connection_result(false, "Unable to save the connection configuration."); }
  return connection_result(true, "Connection saved. It will apply on the next connection.");
}

Json Controller::start(const std::string& mode, Json resolved, bool scanned) {
  if (mode != "desktop" && mode != "vr") return result(false, "Invalid request.");
  if (!search_mode_.empty()) { search_mode_ = mode; mode_ = mode; return result(); }
  if (child_) {
    if (state_ != "starting" && state_ != "streaming") return result(false, "A streaming session is already active.");
    if (active_["backend"].text() == "surfxr") {
      const auto data=read_connection(paths_,false);
      if(data["surfxr"]["version"].text()!="21.0.0-dev14")return result(false,"SURFXR desktop requires v21.");
    }
    if (pending_mode_ == mode || (pending_mode_.empty() && mode_ == mode)) return result();
    try { viewer_command(paths_, mode + "\n"); }
    catch (...) { return result(false, "The streaming process is not ready to change modes yet. Try again shortly."); }
    pending_mode_ = mode; mode_deadline_ = Clock::now() + std::chrono::seconds(40);
    if(mode=="vr"&&active_["backend"].text()=="surfxr"){
      startup_requested_ms_=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
      steamvr_startup_=object({{"state","not_started"},{"reason","none"},{"request_id",int64_t(0)}});
    }
    return result();
  }
  try {
    auto data = read_connection(paths_, false), c = public_connection(data, network(), {}, found_);
    const bool surfxr = data["backend"].text() == "surfxr";
    if(surfxr&&mode=="desktop"){
      if(data["surfxr"]["version"].text()!="21.0.0-dev14")return result(false,"SURFXR desktop requires v21.");
    }
    auto effective = resolved.is(json_type_null) ? c["effective"] : effective_connection(data["profile"].text(), resolved, network());
    if (surfxr && data["surfxr"]["version"].text()=="20.14.1"&&effective["host"].text().empty()) return result(false, "Enter the SURFXR v20 PC IPv4 address and trust this Frame on the PC.");
    if ((!surfxr||data["surfxr"]["version"].text()=="21.0.0-dev14") && resolved.is(json_type_null)) {
      if (effective["host"].text().empty() || (data["remember_last_ip"].boolean() && effective["reason"].text() == "route_unavailable")) return discover(mode);
      if (data["remember_last_ip"].boolean() && !data["last_hosts"][data["profile"].text()].text().empty() && effective["ready"].boolean()) return discover(mode, effective);
    }
    if (!effective["ready"].boolean()) return result(false, "The selected local link is unavailable. Check the connection settings.");
    launch(data, effective, mode, scanned);
  } catch (...) { return result(false, "Connection setup is required, or the streaming process could not start."); }
  return result();
}
Json Controller::request(const Json& request) {
  const auto command = request["command"].text();
  poll();
  if (command == "config_set" && request.only({"command", "profile", "host", "local_address", "profiles", "remember_last_ip", "backend", "surfxr"})) return configure(request);
  if (command == "config_get" && request.only({"command"})) return connection_result();
  if (command == "discover" && request.only({"command"})) return discover();
  constexpr const char* names[] = {"settings", "image_quality", "input_preferences"};
  for (int i = 0; i < 3; ++i) {
    if (command == std::string(names[i]) + "_get" && request.only({"command"})) return preferences(i);
    if (command == std::string(names[i]) + "_set" && request.only({"command", "setting", "value"})) return change_preference(i, request);
  }
  if (command == "start" && request.only({"command", "mode", "profile"})) {
    const auto mode=request["mode"].text("desktop");
    if(mode!="desktop"&&mode!="vr")return result(false,"Invalid request.");
    if(request.has("profile")) {
      const auto profile=request["profile"].text();
      if(!valid_profile(profile))return result(false,"Invalid request.");
      // Selecting a saved profile cannot move an already connected viewer.
      // Refuse that mismatch before changing configuration or reporting success.
      if(child_&&active_["profile"].text()!=profile)
        return result(false,"Disconnect before starting on a different network link.");
      const auto selected=configure(object({{"command","config_set"},{"profile",profile}}));
      if(!selected["ok"].boolean())return selected;
    }
    return start(mode);
  }
  if (!request.only({"command", "mode"})) return result(false, "Invalid request.");
  if (command == "status") return result();
  if (command == "stop") { cancel_search();  if (child_ && state_ != "stopping") begin_stop(true); return result(); }
  return result(false, "Invalid request.");
}
}
