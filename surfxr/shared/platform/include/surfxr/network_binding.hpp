#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>

namespace surfxr {
inline bool valid_local_ipv4(const std::string& ip) noexcept {
    in_addr address{};
    if(inet_pton(AF_INET,ip.c_str(),&address)!=1)return false;
    const auto value=ntohl(address.s_addr);
    return value!=0&&(value>>24)<224;
}
// Bind discovery, its connection trigger and every stream socket to the same
// selected local address. Never retry without the requested binding on error.
inline bool bind_local_ipv4(int fd,const std::string& ip) noexcept {
    if(ip.empty())return true;
    if(!valid_local_ipv4(ip))return false;
    sockaddr_in local{};local.sin_family=AF_INET;
    inet_pton(AF_INET,ip.c_str(),&local.sin_addr);
    return bind(fd,reinterpret_cast<const sockaddr*>(&local),sizeof(local))==0;
}
}
