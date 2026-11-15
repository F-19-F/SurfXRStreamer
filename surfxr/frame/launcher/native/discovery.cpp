#include "controller.hpp"
#include <arpa/inet.h>
#include <csignal>

namespace surfxr::launcher {
void Controller::cancel_search() {
  searches_.clear(); search_mode_.clear(); preferred_search_ = false;
  if (!child_ && state_ == "starting") { state_ = "idle"; message_ = "Ready to connect."; }
}
Json Controller::discover(const std::string& mode, Json preferred) {
  if (!searches_.empty()) return connection_result(false, "PC search is already running.");
  try {
    auto data = read_connection(paths_, false);
    constexpr bool surfxr=true;
    if(surfxr&&data["surfxr"]["version"].text()!="21.0.0-dev14")return connection_result(false,"SURFXR v20 requires a PC IPv4 address and PC-side trust.");
    const auto profile = mode.empty() ? "auto" : data["profile"].text();
    const auto local = preferred.is(json_type_null) ? (mode.empty() ? "" : data["profiles"][profile]["local_address"].text()) : preferred["local_address"].text();
    auto sources = scan_sources(network(), profile, local);
    if (!sources.size()) return connection_result(false, "PC search could not start. Check the selected network.");
    found_ = Json::array(); preferred_search_ = !preferred.is(json_type_null);
    const auto host = preferred["host"].text();
    for (size_t i = 0; i < sources.size(); ++i) {
      auto source = sources.at(i);
      std::vector<std::string> args = {paths_.viewer.parent_path().string() + "/surfxr-discover"};
      if(surfxr)args.insert(args.end(),{"--backend","surfxr"});
      if (host.empty()) args.insert(args.end(), {"--scan", "--local-address", source["address"].text(), "--prefix", std::to_string(source["prefix"].integer())});
      else args.insert(args.end(), {"--host", host, "--local-address", source["address"].text()});
      if(surfxr){
        if(!host.empty())args.insert(args.end(),{"--prefix",std::to_string(source["prefix"].integer())});
        args.insert(args.end(),{"--surfxr-control-port",std::to_string(data["surfxr"][mode=="desktop"?"desktop_port":"control_port"].integer())});
      }
      source.set("discovery_backend","surfxr");
      const unsigned variants=surfxr&&mode.empty()?2u:1u;
      for(unsigned variant=0;variant<variants;++variant){
        auto query=args;if(variant)query.back()=std::to_string(data["surfxr"]["desktop_port"].integer());
        try { searches_.push_back({std::make_unique<Process>(query,"",32768),source,host,Clock::now()+std::chrono::seconds(6)}); }
        catch (...) {}
      }
    }
    if (searches_.empty()) throw std::runtime_error("search_unavailable");
    search_mode_ = mode;
    search_message_ = preferred_search_ ? "Trying the last connected PC address..." : "Searching Wi-Fi and 6 GHz for SURFXR PCs...";
    if (!mode.empty()) { state_ = "starting"; mode_ = mode; message_ = search_message_; }
  } catch (...) { return connection_result(false, "Connection setup is required before searching for PCs."); }
  return connection_result();
}
void Controller::poll_search() {
  const bool had_search = !searches_.empty();
  for (auto it = searches_.begin(); it != searches_.end();) {
    if (!it->child->poll()) { if (Clock::now() >= it->deadline) { it->child->signal(SIGKILL); it->cancelled = true; } ++it; continue; }
    try {
      if (it->cancelled || it->child->code()) throw std::runtime_error("search_failed");
      auto report = Json::parse(it->child->output());
      if (report["status"].text() != (it->host.empty() ? "scan_complete" : "discovered") ||
          !report["connection_started"].is(json_type_boolean) || report["connection_started"].boolean()) throw std::runtime_error("invalid_discovery");
      auto devices = it->host.empty() ? report["devices"] : Json::array(); if (!it->host.empty()) devices.push(report);
      for (size_t i = 0; i < devices.size() && i < 8 && found_.size() < 8; ++i) {
        auto item = devices.at(i); auto address = ipv4(item["source"]);
        if(it->source["discovery_backend"].text()=="surfxr"&&item["backend"].text()!="surfxr")continue;
        if (item["status"].text() != "discovered" || !item["connection_started"].is(json_type_boolean) || item["connection_started"].boolean() ||
            (it->host.empty() && !same_subnet(address, it->source)) || (!it->host.empty() && it->host != address) || address == it->source["address"].text()) continue;
        in_addr host{}, local{}; ::inet_pton(AF_INET, address.c_str(), &host); ::inet_pton(AF_INET, it->source["address"].text().c_str(), &local);
        const auto mask = 0xffffffffu << (32 - it->source["prefix"].integer());
        const auto number = ntohl(host.s_addr), subnet = ntohl(local.s_addr) & mask;
        if (number == subnet || number == (subnet | ~mask)) continue;
        auto choice = object({{"host", address}, {"local_address", it->source["address"]}, {"interface", it->source["name"]}, {"kind", it->source["kind"]}, {"source","surfxr_discovery"}});
        bool duplicate = false; for (size_t j = 0; j < found_.size(); ++j) if (found_.at(j).dump() == choice.dump()) duplicate = true;
        if (!duplicate) found_.push(choice);
      }
    } catch (...) {}
    it = searches_.erase(it);
  }
  if (!searches_.empty() || !had_search) return;
  search_message_ = found_.size() ? "Found PC links. Choose a PC or keep automatic discovery." :
    "No SURFXR PC found. Open the PC SurfXR Dashboard, or enter its IP address.";
  if (search_mode_.empty()) return;
  auto mode = search_mode_; search_mode_.clear();
  if (preferred_search_ && !found_.size()) { preferred_search_ = false; auto r = discover(mode); if (!r["ok"].boolean()) { state_ = "error"; message_ = r["message"].text(); } return; }
  if (found_.size() != 1) { state_ = "error"; message_ = found_.size() ? "Multiple PCs or links found. Choose a PC address before connecting." : search_message_; return; }
  const bool scanned = !preferred_search_;
  auto response = start(mode, found_.at(0), scanned);
  if (!response["ok"].boolean()) { state_ = "error"; message_ = response["message"].text(); }
}
}
