#include "connection.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <future>
#include <map>
#include <regex>
#include <sstream>

namespace surfxr::launcher {
namespace {
uint32_t address_number(const std::string& text) {
  in_addr value{}; if (::inet_pton(AF_INET, text.c_str(), &value) != 1) throw std::runtime_error("invalid_ipv4");
  return ntohl(value.s_addr);
}
bool subnet(uint32_t host, uint32_t source, int prefix) {
  if (prefix < 0 || prefix > 32) return false;
  const uint32_t mask = prefix ? (0xffffffffu << (32 - prefix)) : 0;
  return (host & mask) == (source & mask);
}
Json records(const std::string& text) { try { auto value = Json::parse(text); return value.is(json_type_array) ? value : Json::array(); } catch (...) { return Json::array(); } }
}
bool same_subnet(const std::string& host, const Json& interface) {
  try { return subnet(address_number(host), address_number(interface["address"].text()), static_cast<int>(interface["prefix"].integer(-1))); }
  catch (...) { return false; }
}
Json parse_network(const Json& addresses, const Json& routes, const Json& neighbors, const std::string& wireless) {
  struct Radio { std::string type; int frequency = 0; };
  std::map<std::string, Radio> radios;
  std::string name, line;
  std::istringstream lines(wireless);
  std::smatch match;
  const std::regex iface("\\s*Interface ([A-Za-z0-9_.:-]{1,15})\\s*"), type("\\s*type (\\S+)\\s*"), frequency("channel [0-9]+ \\(([0-9]+) MHz\\)");
  while (std::getline(lines, line)) {
    if (line.find("Unnamed/non-netdev interface") != std::string::npos || line.find("phy#") != std::string::npos) name.clear();
    else if (std::regex_match(line, match, iface)) { name = match[1]; radios[name] = {}; }
    else if (!name.empty()) {
      if (std::regex_match(line, match, type)) radios[name].type = match[1];
      if (std::regex_search(line, match, frequency)) radios[name].frequency = std::stoi(match[1]);
    }
  }
  auto interfaces = Json::array();
  for (size_t i = 0; i < addresses.size() && interfaces.size() < 16; ++i) {
    const auto record = addresses.at(i); name = record["ifname"].text();
    if (name == "lo" || !std::regex_match(name, std::regex("[A-Za-z0-9_.:-]{1,15}"))) continue;
    const auto radio = radios[name];
    const auto kind = radio.type == "AP" ? (radio.frequency >= 5925 ? "direct" : "unknown") : "lan";
    bool up = false; for (size_t n = 0; n < record["flags"].size(); ++n) if (record["flags"].at(n).text() == "UP") up = true;
    auto info = record["addr_info"];
    for (size_t n = 0; n < info.size() && interfaces.size() < 16; ++n) {
      auto item = info.at(n); if (item["family"].text() != "inet") continue;
      try {
        auto local = ipv4(item["local"]); const auto prefix = item["prefixlen"].integer(-1);
        if (prefix < 0 || prefix > 32) continue;
        interfaces.push(object({{"name", name}, {"address", local}, {"prefix", prefix}, {"kind", kind}, {"up", up}}));
      } catch (...) {}
    }
  }
  return object({{"interfaces", interfaces}, {"routes", routes}, {"neighbors", neighbors}});
}
Json local_network() {
  const std::vector<std::vector<std::string>> commands = {{"ip","-j","-4","address","show"}, {"ip","-j","-4","route","show"}, {"ip","-j","-4","neighbor","show"}, {"iw","dev"}};
  std::vector<std::future<std::string>> jobs;
  for (auto command : commands) jobs.push_back(std::async(std::launch::async, [command] {
    try {
      auto r = run(command, 200);
      if (!r.code) return r.output;
      std::fprintf(stderr, "network probe %s %s failed: exit %d\n", command[0].c_str(), command.back().c_str(), r.code);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "network probe %s %s failed: %s\n", command[0].c_str(), command.back().c_str(), error.what());
    }
    return std::string();
  }));
  std::vector<std::string> outputs; for (auto& job : jobs) outputs.push_back(job.get());
  return parse_network(records(outputs[0]), records(outputs[1]), records(outputs[2]), outputs[3]);
}
Json scan_sources(const Json& network, const std::string& profile, const std::string& local) {
  auto eligible = Json::array(), result = Json::array();
  auto interfaces = network["interfaces"];
  for (size_t i = 0; i < interfaces.size(); ++i) {
    auto item = interfaces.at(i); const auto kind = item["kind"].text(), address = item["address"].text();
    if (!item["up"].boolean() || (kind != "lan" && kind != "direct") || (profile != "auto" && kind != profile) || (!local.empty() && local != address)) continue;
    const auto n = address_number(address);
    const auto prefix = item["prefix"].integer();
    const bool private_ip = n >> 24 == 10 || (n & 0xfff00000u) == 0xac100000u || n >> 16 == 0xc0a8;
    if (private_ip && prefix >= 1 && prefix <= 30) eligible.push(item);
  }
  std::vector<std::string> selected;
  const auto add = [&](Json item) { const auto address = item["address"].text(); if (result.size() < 4 && std::find(selected.begin(), selected.end(), address) == selected.end()) { result.push(item); selected.push_back(address); } };
  for (auto kind : {"lan", "direct"}) for (size_t i = 0; i < eligible.size(); ++i) if (eligible.at(i)["kind"].text() == kind) { add(eligible.at(i)); break; }
  for (size_t i = 0; i < eligible.size(); ++i) add(eligible.at(i));
  return result;
}
Json effective_connection(const std::string& profile, const Json& endpoint, const Json& network) {
  auto result = object({{"host", endpoint["host"]}, {"local_address", endpoint["local_address"]}, {"interface", ""}, {"ready", false}, {"reason", "host_required"}});
  auto available = Json::array(), interfaces = network["interfaces"];
  const auto local = endpoint["local_address"].text(), host = endpoint["host"].text();
  for (size_t i = 0; i < interfaces.size(); ++i) {
    auto item = interfaces.at(i);
    if (item["up"].boolean() && (profile == "auto" || item["kind"].text() == profile) && (local.empty() || item["address"].text() == local)) available.push(item);
  }
  if (!available.size()) { result.set("reason", local.empty() ? "link_unavailable" : "local_address_unavailable"); return result; }
  Json chosen;
  int longest = -1; int64_t best_metric = INT64_MAX;
  if (host.empty()) { auto sources = scan_sources(network, profile, local); if (sources.size()) chosen = sources.at(0); }
  else {
    for (size_t i = 0; i < available.size(); ++i) if (same_subnet(host, available.at(i)) && available.at(i)["prefix"].integer() > longest) {
      chosen = available.at(i); longest = static_cast<int>(chosen["prefix"].integer());
    }
    if (chosen.is(json_type_null) && profile != "direct") {
      auto routes = network["routes"];
      for (size_t i = 0; i < routes.size(); ++i) {
        auto route = routes.at(i); auto destination = route["dst"].text("default");
        int prefix = 0; std::string source = "0.0.0.0";
        try { if (destination != "default") { auto slash = destination.find('/'); source = destination.substr(0, slash); prefix = slash == std::string::npos ? 32 : std::stoi(destination.substr(slash+1)); }
          if (route["type"].text("unicast") != "unicast" || !subnet(address_number(host), address_number(source), prefix)) continue;
        } catch (...) { continue; }
        const auto metric = route["metric"].integer();
        for (size_t j = 0; j < available.size(); ++j) if (available.at(j)["name"].text() == route["dev"].text() && (prefix > longest || (prefix == longest && metric < best_metric))) {
          chosen = available.at(j); longest = prefix; best_metric = metric;
        }
      }
    }
  }
  if (chosen.is(json_type_null)) { if (!host.empty()) result.set("reason", "route_unavailable"); return result; }
  result.set("local_address", chosen["address"]); result.set("interface", chosen["name"]); result.set("ready", true); result.set("reason", host.empty() ? "discovery_required" : "ready");
  return result;
}
}
