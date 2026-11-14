#pragma once
#include <array>
#include <cstdint>
#include "surfxr/tracking.hpp"
namespace surfxr {
struct ShortcutButtons {std::array<bool,2> active{},down{};bool system_down=false;};
ShortcutButtons shortcut_buttons(const TrackingFrame&,bool vr_mode,
    const std::array<bool,2>& menu_active,const std::array<bool,2>& menu_down);
struct ShortcutResult {std::array<bool,2> keyboard{};bool toggle_mode=false;};
class ShortcutGesture {
public:
    // Monotonic wall time, independent of render delta. Inactive hands cancel
    // pending gestures and require 30 ms stable release before re-arming.
    // System input cancels both hands; spatial pose is not button validity.
    ShortcutResult update(std::uint64_t now_ns,const std::array<bool,2>& active,const std::array<bool,2>& down,bool system_down=false);
    // Focus/profile changes require a newly observed stable release.
    void reset() noexcept;
    // Logical mode changes consume old clicks, preserving verified release.
    void mode_changed() noexcept;
    bool guarding() const noexcept {return !hands_[0].armed&&!hands_[1].armed;}
    std::uint64_t reset_serial() const noexcept {return reset_serial_;}
private:
    struct Hand {bool sampled=false,raw=false,armed=false,down=false,pending=false,second=false;
        std::uint64_t changed_ns=0,press_ns=0,release_ns=0;};
    std::array<Hand,2> hands_{};std::uint64_t last_ns_=0;bool have_time_=false;
    std::uint64_t reset_serial_=0;
};
}
