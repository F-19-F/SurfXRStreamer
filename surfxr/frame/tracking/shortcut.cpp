#include "surfxr/shortcut.hpp"
namespace surfxr {
ShortcutButtons shortcut_buttons(const TrackingFrame& frame,bool vr_mode,
        const std::array<bool,2>& menu_active,const std::array<bool,2>& menu_down){
    ShortcutButtons result;
    for(unsigned i=0;i<2;++i){const auto& hand=frame.hands[i];
        const bool native=hand.profile==TrackingProfile::SteamFrame;
        // Frame shortcuts only consume the explicit physical right Menu action.
        // Generic application Menu bindings cannot turn System/View into shortcuts.
        const bool valid=native?bool(hand.buttons_active&ButtonMenu):menu_active[i];
        result.active[i]=frame.focused&&valid&&tracking_shortcut_reserved(hand.profile,i,vr_mode);
        result.down[i]=native?bool(hand.buttons&hand.buttons_active&ButtonMenu):menu_down[i];
        result.system_down|=bool(hand.buttons&hand.buttons_active&ButtonSystem);
    }
    return result;
}
void ShortcutGesture::reset() noexcept {hands_={};last_ns_=0;have_time_=false;++reset_serial_;}
void ShortcutGesture::mode_changed() noexcept {
    // Consume old clicks, but a known stable release needs no second lockout.
    for(auto& h:hands_){
        if(h.armed&&!h.raw&&!h.down&&last_ns_-h.changed_ns>=30'000'000){h.pending=false;h.second=false;}
        else h={};
    }
    ++reset_serial_;
}
ShortcutResult ShortcutGesture::update(std::uint64_t now,const std::array<bool,2>& active,const std::array<bool,2>& down,bool system_down){
    ShortcutResult result;
    // A sampling interruption cannot complete an earlier gesture.
    if(!have_time_||now<last_ns_||now-last_ns_>100'000'000){
        hands_={};
    }
    have_time_=true;last_ns_=now;
    constexpr std::uint64_t debounce_ns=30'000'000,double_ns=350'000'000,max_click_ns=500'000'000;
    // System cancels old gestures. Only observed, debounced Menu release can
    // re-arm afterward; inactive time and held buttons cannot satisfy it.
    if(system_down){hands_={};return result;}
    for(unsigned i=0;i<2;++i){auto& h=hands_[i];
        if(!active[i]){h={};continue;}
        if(!h.sampled){h.sampled=true;h.raw=down[i];h.changed_ns=now;continue;}
        if(down[i]!=h.raw){h.raw=down[i];h.changed_ns=now;}
        const bool stable=now-h.changed_ns>=debounce_ns;
        if(!h.armed){if(stable&&!h.raw)h.armed=true;continue;}
        if(stable&&h.raw!=h.down){
            if(h.raw){
                // Debounce must not shrink the physical double-click window.
                h.second=h.pending&&h.changed_ns-h.release_ns<=double_ns;
                if(h.pending&&!h.second)result.keyboard[i]=true;
                h.pending=false;h.press_ns=h.changed_ns;
            }else{
                const auto duration=h.changed_ns-h.press_ns;
                if(duration>=debounce_ns&&duration<=max_click_ns){
                    if(h.second)result.toggle_mode=true;
                    else{h.pending=true;h.release_ns=h.changed_ns;}
                }
                h.second=false;
            }
            h.down=h.raw;
        }
        // A candidate second down within the deadline gets time to settle.
        if(h.pending&&now-h.release_ns>double_ns&&(!h.raw||h.changed_ns-h.release_ns>double_ns)){
            result.keyboard[i]=true;h.pending=false;
        }
    }
    if(result.toggle_mode){mode_changed();result.keyboard={};}
    return result;
}
}
