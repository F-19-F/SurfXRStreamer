#pragma once
#include "dashboard_model.hpp"

namespace surfxr::dashboard {
// Own only this overlay's keyboard. The entered host is validated by the connection service.
template<class Overlay> class HostKeyboard {
 public:
  static constexpr uint64_t tag = 0x564441434354;
  bool open(Overlay* api, vr::VROverlayHandle_t overlay,bool /*host*/=true) {
    if (!api || active_) return false;
    ++user_value_;
    active_ = api->ShowKeyboardForOverlay(overlay, vr::k_EGamepadTextInputModeNormal,
        vr::k_EGamepadTextInputLineModeSingleLine, vr::KeyboardFlag_Modal,
        "SURFXR PC IPv4 address", 15, "", user_value_) == vr::VROverlayError_None;
    return active_;
  }
  bool accept(Overlay* api, const vr::VREvent_t& event, std::string& host) {
    if (event.eventType != vr::VREvent_KeyboardDone && event.eventType != vr::VREvent_KeyboardClosed) return false;
    if (!active_ || !api || event.data.keyboard.uUserValue != user_value_) return false;
    if (event.eventType == vr::VREvent_KeyboardClosed) {
      active_ = false;
      return false;
    }
    if (event.eventType != vr::VREvent_KeyboardDone) return false;
    std::array<char, 16> buffer{};
    const auto length = api->GetKeyboardText(buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (length < buffer.size()) host.assign(buffer.data(), strnlen(buffer.data(), buffer.size()));
    std::fill(buffer.begin(), buffer.end(), '\0');
    cancel(api);
    return true;
  }
  void cancel(Overlay* api) {
    if (active_ && api) api->HideKeyboard();
    active_ = false;
  }
 private:
  bool active_ = false;
  uint64_t user_value_ = tag;
};
} // namespace surfxr::dashboard
