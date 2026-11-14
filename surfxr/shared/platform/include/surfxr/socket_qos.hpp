#pragma once
#include <cstdint>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <qos2.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace surfxr {
// Per-tunnel hints only. OS acceptance does not establish the DSCP seen on the
// wire or the access point's WMM classification. Unsupported QoS is nonfatal.
class SocketQos {
public:
    SocketQos() = default;
    ~SocketQos() { reset(); }
    SocketQos(const SocketQos&) = delete;
    SocketQos& operator=(const SocketQos&) = delete;
    SocketQos(SocketQos&& other) noexcept { *this = std::move(other); }
    SocketQos& operator=(SocketQos&& other) noexcept {
        if (this != &other) {
            reset();
#ifdef _WIN32
            handle_ = std::exchange(other.handle_, nullptr);
#endif
            applied_ = std::exchange(other.applied_, false);
            error_ = std::exchange(other.error_, 0);
            local_priority_error_ = std::exchange(other.local_priority_error_, 0);
        }
        return *this;
    }
    void reset() noexcept {
#ifdef _WIN32
        if (handle_) QOSCloseHandle(std::exchange(handle_, nullptr));
#endif
        applied_ = false;
        error_ = local_priority_error_ = 0;
    }
#ifdef _WIN32
    void apply(SOCKET socket, const sockaddr_in* peer, bool priority) noexcept {
        reset();
        QOS_VERSION version{1, 0};
        if (!QOSCreateHandle(&version, &handle_)) {
            error_ = GetLastError();
            return;
        }
        QOS_FLOWID flow = 0;
        // UDP is deliberately unconnected; qWAVE therefore requires the actual
        // authenticated destination. TCP may also supply its established peer.
        sockaddr_in destination{};
        if (peer) destination = *peer;
        if (!QOSAddSocketToFlow(handle_, socket,
                peer ? reinterpret_cast<sockaddr*>(&destination) : nullptr,
                priority ? QOSTrafficTypeControl : QOSTrafficTypeAudioVideo,
                QOS_NON_ADAPTIVE_FLOW, &flow)) {
            error_ = GetLastError();
            QOSCloseHandle(std::exchange(handle_, nullptr));
            return;
        }
        applied_ = true;
    }
#else
    void apply(int socket, const sockaddr_in*, bool priority) noexcept {
        reset();
        const int tos = (priority ? 46 : 40) << 2;
        if (setsockopt(socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0)
            error_ = static_cast<std::uint32_t>(errno);
        // Explicit local WLAN queue preference: latency-sensitive critical
        // traffic (6) remains above streaming video (5).
        const int local_priority = priority ? 6 : 5;
        if (setsockopt(socket, SOL_SOCKET, SO_PRIORITY, &local_priority, sizeof(local_priority)) != 0)
            local_priority_error_ = static_cast<std::uint32_t>(errno);
        applied_ = error_ == 0 && local_priority_error_ == 0;
    }
#endif
    bool applied() const noexcept { return applied_; }
    std::uint32_t error() const noexcept { return error_; }
    std::uint32_t local_priority_error() const noexcept { return local_priority_error_; }
private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
    bool applied_ = false;
    std::uint32_t error_ = 0, local_priority_error_ = 0;
};
} // namespace surfxr
