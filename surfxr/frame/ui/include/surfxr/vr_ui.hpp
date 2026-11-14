#pragma once
#include "surfxr/remote_settings.hpp"
#include "surfxr/image_quality.hpp"
#include "surfxr/input_preferences.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace surfxr {
enum class UiSettingsPage { Stream, Image, Devices, Diagnostics };
enum class UiDiagnosticsPage { Overlays, Content, Layout, Gaze };
inline constexpr float kUiMinPanelWidthM=.35f;
inline constexpr float kUiMaxPanelWidthM=8.f;
struct DesktopCaptureStatus {bool supported=false;unsigned requested=15,accepted=15;};
struct UiVec3 {float x=0,y=0,z=0;};
struct UiQuat {float x=0,y=0,z=0,w=1;};
struct UiPose {UiVec3 position;UiQuat orientation;};
struct UiLayoutPanel {std::uint32_t source_id=0;UiPose anchor_pose;float width_m=1.8f;bool visible=true;float curvature_radians=0;};
// Positions/orientations are relative to the initial head-yaw anchor, never a
// persisted tracking-origin/world pose. Video aspect is recomputed on startup.
struct UiLayout {
    std::uint32_t version=3;
    std::vector<UiLayoutPanel> panels;
    UiPose keyboard_pose{{0,-.78f,-1.65f},{}};
    float keyboard_width_m=1.55f;
    bool keyboard_visible=true;
    float keyboard_space_width_m=2.05f;
    unsigned keyboard_binding=0; // 0 space, 1 left grip, 2 right grip
    UiPose keyboard_offset{{0,.16f,-.28f},{}};
};
bool validate_ui_layout(const UiLayout& layout) noexcept;
struct UiCrop {float x=0,y=0,width=1,height=1;}; // normalized source pixels
enum class UiEventType {PointerMove,PointerButton,Scroll,Text,Key,Exit,ReleaseAll,
    RemoteSettingChanged,LaunchVr,ExitVr,ToggleMode,Recenter,ImageQualityChanged,InputPreferenceChanged,
    ShowDesktop,AddMonitor,RemoveMonitor,SystemCaptureChanged,ShowDashboard,ShortcutChanged};
// Semantic keys, independent of the transport's scan-code/key-code encoding.
enum class UiKey {
    None,Backspace,Enter,Tab,Escape,Left,Right,Up,Down,Shortcut,
    A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,
    Digit0,Digit1,Digit2,Digit3,Digit4,Digit5,Digit6,Digit7,Digit8,Digit9,
    Grave,Minus,Equal,LeftBracket,RightBracket,Backslash,Semicolon,Apostrophe,Comma,Period,Slash,Space,
    F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,F11,F12,
    PrintScreen,ScrollLock,Pause,Insert,Delete,Home,End,PageUp,PageDown,CapsLock,NumLock,
    LeftShift,RightShift,LeftControl,RightControl,LeftAlt,RightAlt,LeftWindows,RightWindows,Menu,
    Numpad0,Numpad1,Numpad2,Numpad3,Numpad4,Numpad5,Numpad6,Numpad7,Numpad8,Numpad9,
    NumpadDecimal,NumpadDivide,NumpadMultiply,NumpadSubtract,NumpadAdd,NumpadEnter,
    VolumeMute,VolumeDown,VolumeUp,Calculator
};
struct UiEvent {
    UiEventType type=UiEventType::PointerMove;
    std::uint32_t source_id=0,panel_id=0;
    float u=0,v=0,scroll_y=0; // u/v mapped through crop; scroll_y is wheel ticks, 120 per detent
    unsigned button=0; // 0 = left, 1 = right
    bool pressed=false;
    UiKey key=UiKey::None;
    std::string text; // printable ASCII; text events are complete insertions
    SettingChange setting; // Only meaningful for RemoteSettingChanged.
    ImageQualityChange image_quality; // Only meaningful for ImageQualityChanged.
    InputPreferenceChange input_preference; // Only meaningful for InputPreferenceChanged.
    KeyboardShortcut shortcut{}; // ShortcutChanged: button selects the saved slot.
    std::uint64_t candidate=0;
};
using UiEventCallback=std::function<void(const UiEvent&)>;
struct UiPanel {
    std::uint32_t panel_id=0,source_id=0;
    std::string title;
    UiCrop crop;
    UiPose pose;
    float width=1.8f,height=1.12f,header_fraction=0; // video is always full-frame; no embedded chrome
    float curvature_radians=0; // 0..1.8, width is arc length
    unsigned curve_segments=0; // 0 exact cylinder; 2..64 renderer's chord segments
    bool visible=true,available=true;
};
struct UiHand {
    bool active=false,select=false,grip=false,menu=false;
    UiPose aim;
    UiPose attachment;bool attachment_valid=false; // physical grip, aim fallback for older callers
    float stick_x=0,stick_y=0;
    float trigger=-1; // >=0: analog trigger with model hysteresis, ORed with select
    bool right_click=false;
    float squeeze=-1; // >=0: analog grip with hysteresis; <0: legacy grip bool
};
enum class UiHitRegion {Video,Keyboard,TopControls,BottomControls,Candidate};
struct UiHit {
    bool valid=false,keyboard=false;
    std::uint32_t panel_id=0;
    float u=0,v=0,distance=0;
    UiVec3 point;
    UiHitRegion region=UiHitRegion::Video;
};
struct UiKeyRect {float x=0,y=0,width=0,height=0;std::string label,text;UiKey key=UiKey::None;int command=0;bool modifier=false;};
struct UiPanelControls {UiPanel top,bottom;bool visible=false;};
struct UiSettingRect {SettingId id;float x,y,width,height;};
struct UiImageQualityRect {ImageQualityId id;float x,y,width,height;};
UiVec3 ui_rotate(UiQuat q,UiVec3 value);
UiQuat ui_multiply(UiQuat a,UiQuat b);
UiPose ui_compose(UiPose a,UiPose b);
UiPose ui_inverse(UiPose pose);
UiHit ui_hit_panel(const UiPanel& panel,const UiPose& aim);
UiPanel ui_candidate_panel(const UiPanel& keyboard,unsigned width,unsigned height);
UiVec3 ui_panel_point(const UiPanel& panel,float u,float v); // world point on mathematical arc

// All model calls run on the XR thread. Events must be queued nonblocking by
// the caller, never sent to a socket here. reset_anchor captures head yaw once.
class VrUiModel {
public:
    void set_panels(std::vector<UiPanel> panels);
    void reset_anchor(UiPose head);
    void update(const std::array<UiHand,2>& hands,float delta_seconds,const UiEventCallback& events);
    void release_inputs(const UiEventCallback& events);
    // A recognized button command does not require a tracked pointing ray.
    void toggle_keyboard(const UiEventCallback& events);
    UiLayout export_layout() const; // requires an established anchor
    bool restore_layout(const UiLayout& layout) noexcept; // startup only, no input held
    const std::vector<UiPanel>& panels() const {return panels_;}
    void set_aspect(std::uint32_t panel_id,float height_over_width,float header_fraction=0);
    void set_curve_segments(std::uint32_t panel_id,unsigned segments);
    UiPanelControls panel_controls(std::uint32_t panel_id) const;
    void set_available(std::uint32_t panel_id,bool available,const UiEventCallback& events);
    unsigned keyboard_binding() const {return keyboard_binding_;}
    void bind_keyboard(unsigned binding,const UiEventCallback& events={});
    const UiPanel& keyboard_panel() const {return keyboard_;}
    void candidate(unsigned width,unsigned height,std::uint64_t token){candidate_width_=width;candidate_height_=height;candidate_token_=token;}
    UiPanel candidate_panel() const {auto p=ui_candidate_panel(keyboard_,candidate_width_,candidate_height_);p.visible=p.visible&&candidate_token_&&!settings_visible_;return p;}
    const std::array<UiHit,2>& hits() const {return hits_;}
    const std::vector<UiKeyRect>& keys() const {return keys_;}
    void set_remote_settings(const RemoteSettingsSnapshot& settings);
    void set_image_quality(const ImageQualityPreferences& preferences);
    const ImageQualityPreferences& image_quality() const {return image_quality_;}
    void set_input_preferences(const InputPreferences& preferences);
    const InputPreferences& input_preferences() const {return input_preferences_;}
    void set_system_capture_status(bool available,bool active){
        if(system_capture_available_!=available||system_capture_active_!=active){
            system_capture_available_=available;system_capture_active_=active;++revision_;
        }
    }
    void set_desktop_capture_status(DesktopCaptureStatus value){
        if(capture_status_.supported!=value.supported||capture_status_.requested!=value.requested||capture_status_.accepted!=value.accepted){capture_status_=value;++revision_;}
    }
    bool shortcut_recording() const {return shortcut_slot_>=0;}
    const char* shortcut_hint() const;
    std::string shortcut_label(const KeyboardShortcut&) const;
    bool system_capture_available() const {return system_capture_available_;}
    bool system_capture_active() const {return system_capture_active_;}
    UiSettingsPage settings_page() const {return settings_page_;}
    UiDiagnosticsPage diagnostics_page() const {return diagnostics_page_;}
    bool image_quality_page() const {return settings_page_==UiSettingsPage::Image;}
    bool settings_visible() const {return settings_visible_;}
    float settings_scroll_offset() const {return settings_scroll_;}
    void show_settings(bool visible,const UiEventCallback& events={});
    std::array<bool,2> input_capture_mask() const;
    static const std::array<UiSettingRect,9>& settings_rows();
    static const std::array<UiImageQualityRect,3>& image_quality_rows();
    bool shift() const {return shift_;}
    bool exit_requested() const {return exit_;}
    bool anchored() const {return anchored_;}
    std::uint64_t revision() const {return revision_;}
    std::uint64_t drag_updates() const {return drag_updates_;}
    std::uint64_t scale_updates() const {return scale_updates_;}
    void raster_keyboard(std::uint8_t* rgba,unsigned width,unsigned height,unsigned stride) const;
    void raster_controls(std::uint32_t panel_id,bool top,std::uint8_t* rgba,unsigned width,unsigned height,unsigned stride) const;
private:
    unsigned candidate_width_=0,candidate_height_=0;std::uint64_t candidate_token_=0;
    struct HandState {bool active=false,select=false,grip=false,menu=false,armed=false,right_click=false,trigger_down=false,drag_by_grip=false;int drag=-1,adjust=-1;UiPose relative,last_drag_aim;UiPanel adjust_plane;float adjust_u=0,initial_width=0,initial_curve=0,menu_seconds=0,scroll_remainder=0;std::uint32_t scroll_panel=UINT32_MAX;};
    std::vector<UiPanel> panels_;
    UiPanel keyboard_;
    unsigned keyboard_binding_=0;
    UiPose keyboard_offset_{{0,.16f,-.28f},{}},keyboard_space_pose_,keyboard_attachment_;
    float keyboard_space_width_=2.05f;
    void update_keyboard_attachment(const std::array<UiHand,2>&,const std::array<bool,2>&);
    std::vector<UiKeyRect> keys_;
    std::array<HandState,2> hands_{};
    std::array<UiHit,2> hits_{};
    struct DesktopPointerState {
        UiHit hover,quiet_origin,prepared;
        float quiet_seconds=0,prepare_seconds=0;
        bool trigger_started=false;
    };
    std::array<DesktopPointerState,2> desktop_pointers_{};
    UiHit desktop_pointer_hit(std::size_t hand,const UiHand&,UiHit,float delta);
    UiHit desktop_pointer_capture(const UiHand&,float delta,const UiEventCallback&);
    UiEvent desktop_pointer_event(const UiHit&) const;
    UiPose anchor_;
    int button_hand_=-1;
    UiEvent button_event_;
    UiHit press_hit_,drag_origin_;float press_seconds_=0,drag_seconds_=0;bool mouse_down_=false;
    std::array<float,4> controls_remaining_{};
    std::array<std::uint64_t,4> controls_order_{};std::uint64_t controls_clock_=0;
    std::vector<UiKey> held_modifiers_;
    std::uint32_t focus_source_=0,focus_panel_=0;
    bool shift_=false,exit_=false,anchored_=false;
    bool settings_visible_=false,keyboard_before_settings_=false,remote_seen_=false;
    RemoteSettingsSnapshot remote_settings_;
    ImageQualityPreferences image_quality_;
    InputPreferences input_preferences_;
    UiSettingsPage settings_page_=UiSettingsPage::Stream;
    UiDiagnosticsPage diagnostics_page_=UiDiagnosticsPage::Overlays;
    DesktopCaptureStatus capture_status_{};
    int shortcut_slot_=-1;KeyboardShortcut shortcut_draft_{};
    unsigned shortcut_offset_=0;bool shortcut_edit_=false;std::array<float,2> shortcut_repeat_{};
    void scroll_shortcuts(int direction);
    bool activate_shortcut_key(const UiKeyRect&,const UiEventCallback&);
    void save_shortcut(const KeyboardShortcut&,const UiEventCallback&);
    bool system_capture_available_=false,system_capture_active_=false;
    float settings_scroll_=0;
    std::uint64_t revision_=1,drag_updates_=0,scale_updates_=0;
    void layout();
    void build_keys();
    void activate_key(std::size_t key,const UiEventCallback& events);
    void set_visible(std::uint32_t panel_id,bool visible,const UiEventCallback& events);
    void reset_panel(std::uint32_t panel_id);
    int pending_reset_=-2; // -2 none, -1 complete layout, otherwise panel index.
    bool reset_return_settings_=false,reset_return_keyboard_=false;
    void request_reset(int panel,const UiEventCallback&);
    void cancel_reset();
    void raster_reset_confirmation(std::uint8_t*,unsigned,unsigned,unsigned) const;
    void activate_settings(float u,float v,const UiEventCallback& events);
    void scroll_settings(float amount);
    void raster_settings(std::uint8_t* rgba,unsigned width,unsigned height,unsigned stride) const;
};
// Text coordinates refer to the visible ink origin; measurements match raster output.
struct UiTextSize {unsigned width=0,height=0;};
UiTextSize ui_text_size(const std::string& text,unsigned scale);
void ui_text_centered(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,unsigned rw,unsigned rh,const std::string& text,unsigned maximum_scale,std::array<std::uint8_t,4> color);
void ui_fill(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,unsigned rw,unsigned rh,std::array<std::uint8_t,4> color);
void ui_text(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,const std::string& text,unsigned scale,std::array<std::uint8_t,4> color);
} // namespace surfxr
