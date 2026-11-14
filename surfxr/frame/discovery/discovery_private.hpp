#pragma once
#include "surfxr/network_binding.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace surfxr::discovery_detail {
using Byte=unsigned char;
using Bytes=std::vector<Byte>;
constexpr std::size_t kMaxDatagram=65507;
struct View {const Byte* data;std::size_t size;};
struct ProbeError : std::runtime_error {using std::runtime_error::runtime_error;};
inline void require(bool condition,const char* reason){if(!condition)throw ProbeError(reason);}
struct Socket {int fd=-1;~Socket(){if(fd>=0)close(fd);}Socket()=default;Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;};
}
