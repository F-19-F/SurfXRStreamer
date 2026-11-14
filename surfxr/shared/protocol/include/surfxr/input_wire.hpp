#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "surfxr/surfxr_desktop.hpp"

namespace surfxr {
// PC set-1 scan codes; extended keys include the E0 prefix in the high byte.
enum class ScanCode : std::uint16_t {
    Escape=0x01,Backspace=0x0e,Tab=0x0f,Enter=0x1c,
    LeftControl=0x1d,LeftShift=0x2a,RightShift=0x36,LeftAlt=0x38,Space=0x39,LeftWindows=0xe05b,
    Left=0xe04b,Right=0xe04d,Up=0xe048,Down=0xe050,
    Home=0xe047,End=0xe04f,Insert=0xe052,Delete=0xe053,PageUp=0xe049,PageDown=0xe051,
    RightControl=0xe01d,RightAlt=0xe038,RightWindows=0xe05c,Application=0xe05d,
    CapsLock=0x3a,NumLock=0x45,ScrollLock=0x46,PrintScreen=0xe037,Pause=0xe045,
    NumpadEnter=0xe01c,NumpadDivide=0xe035,
    Mute=0xe020,Calculator=0xe021,VolumeDown=0xe02e,VolumeUp=0xe030
};
struct InputMonitorRect {std::uint32_t stream_id=0;std::int32_t x=0,y=0,width=0,height=0;};
struct InputStats {
    std::uint64_t accepted=0,rejected=0,coalesced_moves=0,snapshots=0,release_snapshots=0;
    std::uint64_t queue_overflows=0;
    // Edges in emitted semantic-right snapshots (not queued intents or ACKs).
    std::uint64_t right_button_down_snapshots=0,right_button_up_snapshots=0;
};
// Producers never perform socket I/O. Rejected key/button events trigger a
// priority all-up state, preventing a queue overflow from swallowing releases.
// SURFXR desktop input sends UTF-8 text as Unicode input edges.
// No text or key content is logged.
class SessionInputQueue {
public:
    SessionInputQueue();
    ~SessionInputQueue();
    SessionInputQueue(const SessionInputQueue&)=delete;
    SessionInputQueue& operator=(const SessionInputQueue&)=delete;
    bool pointer_move(std::uint32_t stream_id,float u,float v);
    bool pointer_button(unsigned button,bool pressed); // 0 left, 1 right, 2 middle
    bool candidate_click(std::uint64_t token,float u,float v);
    bool scroll(int horizontal,int vertical); // protocol units; one notch = 15
    bool scroll_wheel(float horizontal,float vertical); // Windows units: 120/notch; fractional values accumulate
    bool key(ScanCode scan,bool pressed);
    bool shortcut(const std::vector<ScanCode>& keys); // Atomic bounded admission, reverse releases.
    bool text_ascii(const std::string& text);
    void use_desktop_events(); // SURFXR mode accepts UTF-8 text as Unicode edges.
    void release_all() noexcept;
    InputStats statistics() const;
    // Worker only: complete current monitor layout from the server config.
    void set_monitors(const std::vector<InputMonitorRect>& monitors);
    // Worker only: consume one semantic event, with priority neutral release.
    // Exactly one transport consumer owns a queue; there is no snapshot timer.
    bool next_desktop_event(transport::desktop::Event& event);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
int input_wire_self_test();
} // namespace surfxr
