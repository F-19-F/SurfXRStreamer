#pragma once
#include "platform.hpp"
#include <functional>

namespace surfxr::launcher {
inline constexpr const char* profiles[] = {"auto", "lan", "direct"};
bool valid_profile(const std::string& name);
std::string ipv4(const Json& value, bool optional = false);
Json normalize_connection(const Json& value, bool allow_unconfigured = true);
Json read_connection(const Paths& paths, bool allow_unconfigured = true);
void write_connection(const Paths& paths, const Json& data);
Json new_connection(const std::string& profile = "direct");
Json parse_network(const Json& addresses, const Json& routes, const Json& neighbors, const std::string& wireless);
Json local_network();
bool same_subnet(const std::string& host, const Json& interface);
Json scan_sources(const Json& network, const std::string& profile = "auto", const std::string& local = "");
Json effective_connection(const std::string& profile, const Json& endpoint, const Json& network);
Json public_connection(const Json& data, const Json& network, const Json& active, const Json& discovered);
}
