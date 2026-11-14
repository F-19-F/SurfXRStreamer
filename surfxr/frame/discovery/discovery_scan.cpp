#include "discovery_scan.hpp"
#include "surfxr_discovery.hpp"
#include <ifaddrs.h>
#include <net/if.h>

namespace surfxr::discovery_detail {
namespace {
unsigned interface_for(const ScanNetwork& network) {
    ifaddrs* list = nullptr;
    require(getifaddrs(&list) == 0, "scan_interface_query_failed");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owned(list, freeifaddrs);
    for (auto* item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || !item->ifa_netmask || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || !(item->ifa_flags & IFF_BROADCAST)) continue;
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        const auto* mask = reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
        if (ntohl(address->sin_addr.s_addr) == network.local && ntohl(mask->sin_addr.s_addr) == network.mask) {
            const auto index = if_nametoindex(item->ifa_name);
            require(index != 0, "scan_interface_unavailable");
            return index;
        }
    }
    throw ProbeError("scan_interface_unavailable");
}

void send_broadcast(int fd, const Bytes& request, const ScanNetwork& network, unsigned index,
                    unsigned port,std::uint32_t target=0) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(port));
    destination.sin_addr.s_addr = htonl(target?target:network.broadcast);
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
    iovec vector{const_cast<Byte*>(request.data()), request.size()};
    msghdr message{};
    message.msg_name = &destination;
    message.msg_namelen = sizeof(destination);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = IPPROTO_IP;
    header->cmsg_type = IP_PKTINFO;
    header->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
    in_pktinfo info{};
    info.ipi_ifindex = static_cast<int>(index);
    info.ipi_spec_dst.s_addr = htonl(network.local);
    std::memcpy(CMSG_DATA(header), &info, sizeof(info));
    const auto sent = sendmsg(fd, &message, MSG_DONTWAIT);
    require(sent >= 0 && static_cast<std::size_t>(sent) == request.size(), "scan_broadcast_failed");
}
}

static std::string scan_impl(const char* local,unsigned prefix,const char*& stage,unsigned surfxr_port,const std::string& host) {
    stage = "scan_arguments";
    const auto network = scan_network(local, prefix);
    const auto index = interface_for(network);
    SurfxrReplies surfxr(surfxr_port);const auto& request=surfxr.request;
    in_addr directed{};
    if(!host.empty())require(inet_pton(AF_INET,host.c_str(),&directed)==1&&
        (ntohl(directed.s_addr)&network.mask)==network.network,"scan_host_outside_link");
    const auto target=ntohl(directed.s_addr);
    stage = "udp_scan";
    Socket socket;
    socket.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    require(socket.fd >= 0, "scan_socket_failed");
    int enabled = 1;
    require(setsockopt(socket.fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(socket.fd, IPPROTO_IP, IP_PKTINFO, &enabled, sizeof(enabled)) == 0,
            "scan_socket_options_failed");
    require(surfxr::bind_local_ipv4(socket.fd, local), "scan_local_bind_failed");
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(3);
    auto next_send = start;
    unsigned sent = 0, received = 0;
    Bytes buffer(kMaxDatagram);
    while (std::chrono::steady_clock::now() < deadline && received < 64) {
        const auto now = std::chrono::steady_clock::now();
        if (sent < 3 && now >= next_send) {
            send_broadcast(socket.fd,request,network,index,surfxr_port,target);
            ++sent;
            next_send = start + std::chrono::milliseconds(800 * sent);
        }
        pollfd event{socket.fd, POLLIN, 0};
        const auto ready = poll(&event, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        require(ready >= 0 && !(event.revents & (POLLERR | POLLNVAL)), "scan_wait_failed");
        if (!(event.revents & POLLIN)) continue;
        sockaddr_in sender{};
        iovec vector{buffer.data(), buffer.size()};
        alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
        msghdr message{};
        message.msg_name = &sender;
        message.msg_namelen = sizeof(sender);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const auto count = recvmsg(socket.fd, &message, MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        require(count >= 0, "scan_receive_failed");
        ++received;
        if (count > static_cast<ssize_t>(buffer.size()) || (message.msg_flags & MSG_CTRUNC)) continue;
        bool same_interface = false;
        for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != IPPROTO_IP || header->cmsg_type != IP_PKTINFO ||
                header->cmsg_len < CMSG_LEN(sizeof(in_pktinfo))) continue;
            in_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(header), sizeof(info));
            same_interface = info.ipi_ifindex == static_cast<int>(index);
        }
        const auto ip=ntohl(sender.sin_addr.s_addr);
        if(surfxr_port){
            if(same_interface&&(ip&network.mask)==network.network&&ip!=network.local&&
               ip!=network.network&&ip!=network.broadcast&&(!target||target==ip))
                surfxr.accept({buffer.data(),static_cast<std::size_t>(count)},sender);
        }
    }
    const auto& reports=surfxr.reports;
    if(!host.empty())return reports.empty()?"{\"status\":\"not_found\",\"connection_started\":false}":reports.front();
    std::string report = "{\"status\":\"scan_complete\",\"connection_started\":false,\"devices\":[";
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (i) report += ',';
        report += reports[i];
    }
    return report + "]}";
}
std::string scan_surfxr(const char* local,unsigned prefix,unsigned port,const std::string& host,const char*& stage){
    require(port>=1024&&port<=65535,"invalid_scan_port");return scan_impl(local,prefix,stage,port,host);
}
} // namespace surfxr::discovery_detail
