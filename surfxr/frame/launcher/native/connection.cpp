#include "surfxr/ports.hpp"
#include "connection.hpp"
#include "surfxr/remote_settings.hpp"
#include <arpa/inet.h>
#include <cctype>
#include <glib.h>

namespace surfxr::launcher {
bool valid_profile(const std::string& name) { return name == "auto" || name == "lan" || name == "direct"; }
std::string ipv4(const Json& value, bool optional) {
  if (!value.is(json_type_string)) throw std::runtime_error("invalid_ipv4");
  const auto text = value.text();
  if (optional && text.empty()) return "";
  in_addr address{};
  if (text.find('\0') != std::string::npos || ::inet_pton(AF_INET, text.c_str(), &address) != 1) throw std::runtime_error("invalid_ipv4");
  const auto n = ntohl(address.s_addr);
  if (!n || n == 0xffffffffu || n >> 24 == 127 || (n & 0xf0000000u) == 0xe0000000u) throw std::runtime_error("invalid_ipv4");
  return text;
}

Json new_connection(const std::string& selected) {
  auto endpoints = Json::object(), last = Json::object();
  for (auto name : profiles) { endpoints.set(name, object({{"host", ""}, {"local_address", ""}})); last.set(name, ""); }
  return object({{"version", 3}, {"profile", selected}, {"host", ""}, {"profiles", endpoints},
    {"backend", "surfxr"}, {"surfxr", object({{"version", "21.0.0-dev14"}, {"view_width", static_cast<int>(kDefaultVrViewWidth)}, {"view_height", static_cast<int>(kDefaultVrViewHeight)}, {"control_port", static_cast<int>(ports::vr_control)}, {"desktop_port", static_cast<int>(ports::desktop_control)}})},
    {"remember_last_ip", true}, {"last_hosts", last}});
}
Json normalize_connection(const Json& value, bool allow_unconfigured) {
  (void)allow_unconfigured;
  if (!value.is(json_type_object)) throw std::runtime_error("invalid_connection_config");
  auto result = new_connection(value["profile"].text("auto"));
  if (!valid_profile(result["profile"].text())) throw std::runtime_error("invalid_connection_profile");
  const auto backend = value.has("backend") ? value["backend"] : Json("surfxr");
  if (!backend.is(json_type_string) || backend.text() != "surfxr") throw std::runtime_error("invalid_backend");
  result.set("backend", backend);
  auto surfxr = result["surfxr"];
  if (value.has("surfxr")) {
    const auto fields = value["surfxr"];
    if (!fields.only({"version", "view_width", "view_height", "control_port", "desktop_port"})) throw std::runtime_error("invalid_surfxr_config");
    for (const auto& key : fields.keys()) surfxr.set(key, fields[key]);
    if (!surfxr["version"].is(json_type_string) || (surfxr["version"].text() != "20.14.1" && surfxr["version"].text() != "21.0.0-dev14")) throw std::runtime_error("invalid_surfxr_version");
    for (auto key : {"view_width", "view_height", "control_port"}) {
      const auto n = surfxr[key].integer();
      if (!surfxr[key].is(json_type_int) || n < (std::string(key) == "control_port" ? 1024 : 32) ||
          n > (std::string(key) == "control_port" ? 65535 : std::string(key) == "view_width" ? 4096 : 8192)) throw std::runtime_error("invalid_surfxr_config");
    }
    if (surfxr["view_width"].integer()*surfxr["view_height"].integer() > 16*1024*1024) throw std::runtime_error("invalid_surfxr_extent");
    if(!surfxr["desktop_port"].is(json_type_int)||surfxr["desktop_port"].integer()<1024||surfxr["desktop_port"].integer()>65533)throw std::runtime_error("invalid_surfxr_desktop_port");
  }
  if (surfxr["control_port"].integer() == 9943) surfxr.set("control_port", static_cast<int>(ports::vr_control));
  if (surfxr["desktop_port"].integer() == 9950) surfxr.set("desktop_port", static_cast<int>(ports::desktop_control));
  // Saved historical revisions upgrade to the sole SurfXR product protocol.
  surfxr.set("version","21.0.0-dev14");
  result.set("surfxr", surfxr);
  const auto legacy = value.has("host") ? ipv4(value["host"], true) : "";
  auto saved = value.has("profiles") ? value["profiles"] : Json::object();
  if (!saved.is(json_type_object)) throw std::runtime_error("invalid_connection_profiles");
  auto endpoints = Json::object();
  for (auto name : profiles) {
    auto item = saved.has(name) ? saved[name] : object({{"host", std::string(name) == "direct" ? "" : legacy}, {"local_address", ""}});
    if (!item.is(json_type_object)) throw std::runtime_error("invalid_connection_profile");
    endpoints.set(name, object({{"host", item.has("host") ? ipv4(item["host"], true) : ""},
                               {"local_address", item.has("local_address") ? ipv4(item["local_address"], true) : ""}}));
  }
  auto last = result["last_hosts"];
  if (value.has("last_hosts")) {
    auto fields = value["last_hosts"];
    if (!fields.only({"auto", "lan", "direct"})) throw std::runtime_error("invalid_remembered_connection");
    for (const auto& key : fields.keys()) last.set(key, ipv4(fields[key], true));
  }
  const auto remember = value.has("remember_last_ip") ? value["remember_last_ip"] : Json(true);
  if (!remember.is(json_type_boolean)) throw std::runtime_error("invalid_remembered_connection");
  result.set("profiles", endpoints); result.set("last_hosts", last); result.set("remember_last_ip", remember);
  result.set("host", endpoints[result["profile"].text()]["host"]);
  return result;
}
Json read_connection(const Paths& paths, bool allow_unconfigured) {
  return normalize_connection(Json::parse(read_file(paths.config, 16384, true)), allow_unconfigured);
}
void write_connection(const Paths& paths, const Json& data) {
  auto normalized = normalize_connection(data);
  private_directory(paths.config.parent_path());
  atomic_write(paths.config, normalized.dump() + "\n");
}
Json public_connection(const Json& data, const Json& network, const Json& active, const Json& discovered) {
  const auto selected = data["profile"].text();
  auto preferred = data["profiles"][selected];
  const bool surfxr = data["backend"].text() == "surfxr";
  auto candidates = discovered;
  if (!candidates.is(json_type_array)) candidates = Json::array();
  for (auto name : profiles) {
    const auto remembered = data["last_hosts"][name].text();
    if (data["remember_last_ip"].boolean() && !remembered.empty() && candidates.size() < 16)
      candidates.push(object({{"host", remembered}, {"interface", ""}, {"kind", name}, {"source", "remembered"}}));
    const auto saved = data["profiles"][name]["host"].text();
    if (!saved.empty() && candidates.size() < 16) candidates.push(object({{"host", saved}, {"interface", ""}, {"kind", "unknown"}, {"source", "saved"}}));
  }
  // Passive ARP entries remain hints only. Exclude local addresses and routers;
  // only authenticated surfxr-discover results can initiate automatic startup.
  const auto neighbors = network["neighbors"], interfaces = network["interfaces"], routes = network["routes"];
  for (size_t i = 0; i < neighbors.size() && candidates.size() < 16; ++i) {
    auto entry = neighbors.at(i); std::string host;
    try { host = ipv4(entry["dst"]); } catch (...) { continue; }
    auto states = entry["state"]; if (states.is(json_type_string)) { auto one = states; states = Json::array(); states.push(one); }
    bool invalid = !states.size();
    for (size_t j = 0; j < states.size(); ++j) { auto state = states.at(j).text(); if (state == "FAILED" || state == "INCOMPLETE" || state == "NOARP") invalid = true; }
    for (size_t j = 0; j < interfaces.size(); ++j) if (interfaces.at(j)["address"].text() == host) invalid = true;
    for (size_t j = 0; j < routes.size(); ++j) if (routes.at(j)["gateway"].text() == host) invalid = true;
    for (size_t j = 0; j < candidates.size(); ++j) if (candidates.at(j)["host"].text() == host) invalid = true;
    if (invalid) continue;
    for (size_t j = 0; j < interfaces.size(); ++j) {
      const auto source = interfaces.at(j);
      if (source["up"].boolean() && source["name"].text() == entry["dev"].text() && same_subnet(host,source)) {
        candidates.push(object({{"host",host},{"interface",source["name"]},{"kind",source["kind"]},{"source","neighbor"}})); break;
      }
    }
  }
  if (data["remember_last_ip"].boolean() && !data["last_hosts"][selected].text().empty()) preferred.set("host", data["last_hosts"][selected]);
  auto effective = effective_connection(selected, preferred, network);
  if (surfxr && data["surfxr"]["version"].text()=="20.14.1" && effective["host"].text().empty()) { effective.set("ready", false); effective.set("reason", "host_required"); }
  return object({{"profile", selected}, {"profiles", data["profiles"]},
    {"backend", data["backend"]}, {"surfxr", data["surfxr"]},
    {"interfaces", network["interfaces"]}, {"effective", effective},
    {"candidates", candidates}, {"remember_last_ip", data["remember_last_ip"]}, {"last_hosts", data["last_hosts"]}, {"active", active}});
}
}
