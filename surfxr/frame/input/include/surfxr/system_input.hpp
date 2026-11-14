#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace surfxr::frame_input {
inline constexpr std::uint32_t magic=0x56445349,version=2;
inline constexpr std::uint64_t lease_ns=250'000'000,fresh_ns=100'000'000;
struct Bone {std::array<float,3> position{};std::array<float,4> orientation{0,0,0,1};};
struct Skeleton {std::uint64_t sampled_ns=0;std::array<Bone,31> bones{};};
bool valid_skeleton(const Skeleton&) noexcept;
struct Request {std::uint32_t tag=magic,revision=version,capture=0,reserved=0;std::uint64_t sampled_ns=0;};
struct Snapshot {
    std::uint32_t tag=magic,revision=version;
    std::uint64_t sampled_ns=0,exit_serial=0;
    std::uint32_t available=0,clicks=0,touches=0,touch_available=0,capture=0,reserved=0;
    std::array<Skeleton,2> skeletons{};
};
static_assert(sizeof(Request)==24&&sizeof(Snapshot)==1808,"local input IPC ABI");
std::uint64_t monotonic_ns() noexcept;
std::string socket_path();
int private_directory(); // Verify same-UID, private, real runtime directory.
bool same_user(int socket) noexcept;

// Used inside vrserver as well as by CPU tests. Only system components pass
// through this state machine. All other buttons continue along their old route.
class CapturePolicy {
public:
    void request(bool capture,std::uint64_t now) noexcept;
    bool update(unsigned hand,bool touch,bool down,std::uint64_t now) noexcept;
    void advance(std::uint64_t now,unsigned available,unsigned touch_available) noexcept;
    Snapshot snapshot(std::uint64_t now) const noexcept;
    bool forwarded(unsigned hand,bool touch) const noexcept;
    void stop() noexcept;
private:
    struct Hand {bool click=false,touch=false,swallow=false;unsigned taps=0;std::uint64_t down_ns=0,first_tap_ns=0;};
    std::array<Hand,2> hands_{};
    unsigned available_=0,touch_available_=0;
    bool requested_=false,active_=false,disarmed_=false;
    std::uint64_t heartbeat_=0,exit_serial_=0;
    void leave() noexcept;
};

// Nonblocking, same-user local IPC. Each new VR stream arms one automatic
// entry. Explicit exit or a release after entry requires a new menu request.
class SystemInputClient {
public:
    SystemInputClient()=default;
    SystemInputClient(const SystemInputClient&)=delete;
    SystemInputClient& operator=(const SystemInputClient&)=delete;
    ~SystemInputClient();
    void session(bool active) noexcept;
    void request(bool capture) noexcept {requested_=capture;auto_pending_=false;}
    Snapshot poll(bool eligible,std::uint64_t now) noexcept;
    bool requested() const noexcept {return requested_;}
    // Focus/Stage changes release an attempted capture, while an initial
    // automatic entry can still wait for the first eligible sample.
    void suspend() noexcept;
    void close() noexcept;
private:
    int socket_=-1;
    bool requested_=false,have_snapshot_=false,awaiting_=false,last_request_=false;
    bool session_active_=false,auto_pending_=false;
    std::uint64_t pending_since_=0;
    std::uint64_t retry_ns_=0,last_exit_=0;
    Snapshot latest_{};
    void disconnect() noexcept;
};
}
