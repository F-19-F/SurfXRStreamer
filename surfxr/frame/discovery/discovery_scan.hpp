#pragma once
#include "discovery_private.hpp"

namespace surfxr::discovery_detail {
struct ScanNetwork {
    std::uint32_t local, mask, network, broadcast;
};
inline ScanNetwork scan_network(const char* local, unsigned prefix) {
    in_addr address{};
    require(prefix >= 1 && prefix <= 30 && inet_pton(AF_INET, local, &address) == 1,
            "invalid_scan_network");
    const auto ip = ntohl(address.s_addr);
    require((ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8,
            "scan_requires_private_ipv4");
    const auto mask = 0xffffffffu << (32 - prefix);
    ScanNetwork result{ip, mask, ip & mask, (ip & mask) | ~mask};
    require(ip != result.network && ip != result.broadcast, "invalid_scan_source");
    return result;
}

}
