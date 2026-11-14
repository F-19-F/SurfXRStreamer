#include "controller.hpp"
#include "session_failure.hpp"
#include <cmath>
#include <csignal>
#include <regex>
#include <thread>
#include <cstring>
#include <utility>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace surfxr::launcher {
namespace {
Json numeric(const Json& value, unsigned depth = 0) {
  auto result = Json::object(); if (depth >= 12) return result;
  std::vector<std::string> keys = value.keys();
  if (value.is(json_type_array)) for (size_t i = 0; i < value.size(); ++i) keys.push_back(std::to_string(i));
  for (const auto& key : keys) {
    if (!std::regex_match(key, std::regex("[a-z_0-9]{1,64}"))) continue;
    auto item = value.is(json_type_array) ? value.at(static_cast<size_t>(std::stoul(key))) : value[key];
    if (item.is(json_type_boolean) || item.is(json_type_int) || (item.is(json_type_double) && std::isfinite(item.number()))) result.set(key, item);
    else if (item.is(json_type_object) || item.is(json_type_array)) result.set(key, numeric(item, depth + 1));
  }
  return result;
}
}
void Controller::launch(Json data, Json effective, const std::string& mode, bool /*scanned*/) {
  dashboard_start_.reset();dashboard_deadline_={};
  { Lock viewer(paths_.runtime/"viewer.lock"); } // Reject manually owned viewers before replacing their progress file.
  fs::remove(paths_.runtime / "viewer-status.json");
  const auto host = effective["host"].text(), local = effective["local_address"].text();
  std::vector<std::string> args = {paths_.viewer.string(), "--host", host, "--seconds", "0", "--decoder", "v4l2", "--cpu-affinity", "available", "--status-file", (paths_.runtime/"viewer-status.json").string()};
  if (!local.empty()) { args.push_back("--local-address"); args.push_back(local); }
  {
    args.insert(args.end(), {"--backend", "surfxr", "--surfxr-version", data["surfxr"]["version"].text()});
    args.insert(args.end(),{"--surfxr-desktop-port",std::to_string(data["surfxr"]["desktop_port"].integer())});
    if(mode=="desktop")args.push_back("--surfxr-desktop");
    for (auto key : {"view_width", "view_height", "control_port"}) {
      std::string option = key; std::replace(option.begin(), option.end(), '_', '-');
      args.insert(args.end(), {"--surfxr-"+option, std::to_string(data["surfxr"][key].integer())});
    }
  }
  child_ = std::make_unique<Process>(args, "", 256 * 1024);
  active_ = effective; active_.set("profile", data["profile"]); active_.set("backend", data["backend"]);
  host_ = host; configured_ = true; mode_ = mode; state_ = "starting"; message_ = "Connecting to the PC.";
  progress_ = {}; last_exit_ = {}; failure_ = {}; pending_mode_.clear(); stop_error_.clear(); stop_requested_ = false;
  startup_requested_ms_=0;
  if(mode=="vr")steamvr_startup_=object({{"state","not_started"},{"reason","none"},{"request_id",int64_t(0)}});
  for (auto& pending : pending_) pending = {};
   
}
void Controller::begin_stop(bool normal, const std::string& error) {
  if (!child_ || state_ == "stopping") return;
  stop_requested_ = normal; stop_error_ = error; pending_mode_.clear();
  for (auto& pending : pending_) pending.change = {};
  state_ = "stopping"; message_ = error.empty() ? "Disconnecting." : error;
  stop_stage_ = 0; stop_deadline_ = Clock::now() + std::chrono::seconds(8); child_->signal(SIGINT);
}
void Controller::finish_child() {
  const bool remote_surfxr_end=active_["backend"].text()=="surfxr"&&!stop_requested_;
  const int code = child_->code(); bool valid = false;
  if(!stop_requested_)try{update_startup(Json::parse(read_file(paths_.runtime/"viewer-status.json",16384)));}catch(...){}
  Json diagnostics = Json::object(), report;
  try {
    report = Json::parse(child_->output()); valid = report.is(json_type_object); diagnostics = numeric(report);
  } catch (...) {}
  failure_ = !stop_requested_ && code != 0 ? session_failure(report,code) : Json();
  Json recovered_failure;
  const auto recovered = report["receiver"]["last_vr_failure"];
  if (recovered.is(json_type_object)) recovered_failure = session_failure(object({{"receiver",recovered}}),1);
  const auto finished_at = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  diagnostics.set("launcher", object({{"last_exit", code}, {"report_valid", valid}, {"report_oversized", child_->oversized()},
      {"failure",failure_}, {"recovered_vr_failure",recovered_failure},
      {"finished_at_unix_ms", Json(static_cast<int64_t>(finished_at))}}));
  try {
    private_directory(paths_.state); const auto saved = diagnostics.dump()+"\n";
    if (saved.size() <= 256 * 1024) {
      atomic_write(paths_.state/"last-session.json", saved);
      // Keep the most recent unexpected exit through subsequent successful
      // reconnects. Both records contain only numeric and fixed diagnostics.
      if (!failure_.is(json_type_null) || !recovered_failure.is(json_type_null))
        atomic_write(paths_.state/"last-failure.json", saved);
    }
  } catch (...) {}
  state_ = stop_requested_ || code == 0 ? "idle" : "error";
  message_ = state_ == "idle" ? (report["receiver"]["server_stopped"].boolean()?"PC SteamVR exited. Ready to reconnect.":"Ready to connect.") : failure_["detail"].text();
  if (!stop_error_.empty()) message_ = stop_error_;
  child_.reset(); active_ = {}; progress_ = {}; last_exit_ = Json(code); pending_mode_.clear(); 
  if(remote_surfxr_end){
    // Viewer has released XR and its instance lock before reopening the existing overlay.
    try { dashboard_start_=std::make_unique<Process>(std::vector<std::string>{"systemctl","--user","start","surfxr-dashboard.service"},"",1024);
      dashboard_deadline_=Clock::now()+std::chrono::seconds(8); }catch(...){}
  }
  for (auto& pending : pending_) pending.change = {};
}
void Controller::poll_dashboard(){
  if(dashboard_deadline_==Time{})return;
  if(Clock::now()>=dashboard_deadline_){dashboard_start_.reset();dashboard_deadline_={};return;}
  if(dashboard_start_){if(!dashboard_start_->poll())return;const auto code=dashboard_start_->code();dashboard_start_.reset();
    if(code){dashboard_deadline_={};return;}}
  const auto endpoint=(paths_.runtime/"overlay.sock").string();struct stat st{};
  if(::lstat(endpoint.c_str(),&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=::getuid()||(st.st_mode&0077))return;
  sockaddr_un address{};address.sun_family=AF_UNIX;if(endpoint.size()>=sizeof(address.sun_path))return;
  std::memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);
  Fd socket(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
  if(socket.get()>=0&&::sendto(socket.get(),"show",4,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof(address))==4)dashboard_deadline_={};
}
void Controller::poll() {
  poll_dashboard();
  poll_search();
  if (!child_) return;
  if (child_->poll()) { finish_child(); return; }
  if (state_ == "stopping") {
    if (Clock::now() >= stop_deadline_) { child_->signal(stop_stage_++ == 0 ? SIGTERM : SIGKILL); stop_deadline_ = Clock::now() + std::chrono::seconds(3); }
    return;
  }
  try {
    auto progress = Json::parse(read_file(paths_.runtime/"viewer-status.json", 16384));
    if (progress.is(json_type_object)) progress_ = progress;
    update_startup(progress);
    if (progress["state"].text() == "streaming") {
      if (state_ != "streaming") {
        try { auto data = read_connection(paths_); if (data["remember_last_ip"].boolean()) {
          auto last = data["last_hosts"]; last.set(active_["profile"].text(), active_["host"]); data.set("last_hosts", last); write_connection(paths_, data);
        } } catch (...) { search_message_ = "Connected, but the last PC address could not be saved."; }
      }
      state_ = "streaming"; message_ = "Streaming is running."; 
    }
    const auto mode = progress["mode"].text(); if (mode == "desktop" || mode == "vr") mode_ = mode;
    if (pending_mode_ == mode_) pending_mode_.clear();
    if(pending_mode_=="vr"&&mode_=="desktop"&&steamvr_startup_["state"].text()=="failed"&&
       !progress["last_transition_pending"].boolean())pending_mode_.clear();
  } catch (...) {}
  poll_preferences();

}
void Controller::shutdown() {
  dashboard_start_.reset();dashboard_deadline_={};
  cancel_search(); 
  if (!child_) return;
  begin_stop(true);
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  while (child_ && Clock::now() < deadline) { poll(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  if (child_) { child_->signal(SIGKILL); child_.reset(); }
}
}
