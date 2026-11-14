#include "dashboard_runtime.hpp"
#include "surfxr/battery.hpp"

namespace surfxr::dashboard {
int run(bool show) {
  const std::string directory = runtime_directory();
  Singleton singleton(directory);
  if (!singleton.owns()) {
    if (show && !singleton.show_existing()) throw std::runtime_error("The running dashboard did not respond.");
    return 0;
  }
  std::signal(SIGTERM, stop_signal);
  std::signal(SIGINT, stop_signal);
  std::signal(SIGPIPE, SIG_IGN);
  Dashboard dashboard;
  Canvas canvas(width, height);
  Status status;
  surfxr::BatteryMonitor battery_monitor;
  std::string host_entry_notice;
  bool editing_host=false;
  std::string editing_profile;
  int hover = -1, pressed = -1;
  bool dirty = true, first_texture = true;
  auto language=i18n::system_language();
  auto next_connect = Clock::time_point::min(), next_status = Clock::time_point::min();
  auto next_config = Clock::time_point::min(), next_settings = Clock::time_point::min();
  auto next_upload = Clock::time_point::min();
  while (!stop_requested) {
    show = singleton.take_show() || show;
    auto now = Clock::now();
    const auto updated_language=i18n::system_language();
    if(updated_language!=language){language=updated_language;dirty=true;}
    if (!dashboard.connected() && now >= next_connect) {
      if (dashboard.connect()) {
        first_texture = dirty = true;
        next_status = Clock::time_point::min();
        next_upload = Clock::time_point::min();
      }
      next_connect = now + std::chrono::seconds(3);
    }
    if (dashboard.connected() && dashboard.runtime_quit()) {
      dashboard.disconnect();
      hover = pressed = -1;
      next_connect = now + std::chrono::seconds(3);
    }
    // Do not open a newly attached tab until it has a complete image.
    if (show && dashboard.connected() && !first_texture) { dashboard.show(); show = false; }
    const bool visible = dashboard.visible();
    if (now >= next_status) {
      auto updated = merge_status(request(directory, "status"), status);
      const auto battery=battery_monitor.raw_headset();
      updated.raw_battery_percent=battery.percent;updated.raw_battery_charging=battery.charging;
      if (!(updated == status)) { status = std::move(updated); dirty = true; }
      next_status = Clock::now() + (visible ? std::chrono::seconds(1) : std::chrono::seconds(5));
    }
    // Configuration discovery is only needed while the tab is visible. It does
    // not start a session or cause hidden texture uploads.
    if ((visible || first_texture) && status.available && now >= next_config) {
      auto updated = merge_status(request(directory, "config_get"), status);
      if (!(updated == status)) { status = std::move(updated); dirty = true; }
      next_config = Clock::now() + std::chrono::seconds(5);
    }
    if ((visible || first_texture) && status.available && now >= next_settings) {
      auto updated = merge_status(request(directory, "settings_get"), status);
      updated = merge_status(request(directory, "image_quality_get"), updated);
      updated = merge_status(request(directory, "input_preferences_get"), updated);
      if (!(updated == status)) { status = std::move(updated); dirty = true; }
      next_settings = Clock::now() + std::chrono::seconds(1);
    }
    vr::VREvent_t event{};
    for (int events = 0; events != 128 && dashboard.event(event); ++events) {
      std::string host_entry;
      if (dashboard.accept_host_entry(event, host_entry)) {
        status = merge_status(save_connection(directory,editing_profile.c_str(),
            {host_entry,status.connection.profiles[profile_index(editing_profile)].local_address,"",""}), status);
        std::fill(host_entry.begin(), host_entry.end(), '\0');
        host_entry_notice = status.message;
        dirty = true;
        next_config = Clock::time_point::min();
      }
      if (event.eventType == vr::VREvent_Quit) {
        if (vr::VRSystem()) vr::VRSystem()->AcknowledgeQuit_Exiting();
        dashboard.disconnect();
        hover = pressed = -1;
        next_connect = Clock::now() + std::chrono::seconds(3);
        break;
      }
      if (event.eventType == vr::VREvent_OverlayShown) {
        dirty = true;
        next_status = Clock::time_point::min();
        next_config = next_settings = Clock::time_point::min();
      }
      if (event.eventType == vr::VREvent_OverlayHidden || event.eventType == vr::VREvent_DashboardDeactivated) {
        if (event.eventType == vr::VREvent_DashboardDeactivated) dashboard.cancel_host_entry();
        hover = pressed = -1;
        dirty = true;
      }
      if (event.eventType == vr::VREvent_MouseMove || event.eventType == vr::VREvent_MouseButtonDown ||
          event.eventType == vr::VREvent_MouseButtonUp) {
        const int over = hit(status, event.data.mouse.x, height - event.data.mouse.y);
        if (hover != over) { hover = over; dirty = true; }
        if (event.data.mouse.button != vr::VRMouseButton_Left && event.eventType != vr::VREvent_MouseMove) continue;
        if (event.eventType == vr::VREvent_MouseButtonDown) {
          pressed = over >= 0 && enabled(status, over) ? over : -1;
          dirty = true;
        }
        if (event.eventType == vr::VREvent_MouseButtonUp) {
          if (pressed >= 0 && pressed == over && enabled(status, pressed)) {
            if (pressed == 28) {
              dashboard.cancel_host_entry();
              editing_host=true;editing_profile=status.connection.profile;
              host_entry_notice = dashboard.edit_host_entry(editing_host) ? "Enter the PC IPv4 address, then press Done." :
                  "SteamVR keyboard unavailable. Try again or use the desktop settings.";
            } else {
              if(pressed==31){dashboard.cancel_host_entry();host_entry_notice.clear();}
              status = activate(directory, status, pressed);
            }
            if (stop_requested) break;
            next_status = Clock::now() + std::chrono::seconds(1);
            if (pressed >= 8) next_settings = Clock::now() + std::chrono::milliseconds(500);
          }
          pressed = -1;
          dirty = true;
        }
      }
    }
    if (stop_requested) break;
    if (dashboard.connected() && dirty && (visible || first_texture) && Clock::now() >= next_upload) {
      render(canvas, status, hover, pressed, host_entry_notice);
      const auto result = dashboard.upload(canvas);
      if (result == UploadResult::reconnect) {
        dashboard.disconnect();
        next_connect = Clock::now() + std::chrono::seconds(3);
      } else if (result == UploadResult::published) {
        dirty = first_texture = false;
      } else {
        // A transient failure must not destroy the tab or clear its last image.
        // Keep the newest UI state dirty and retry without spinning every tick.
        next_upload = Clock::now() + std::chrono::milliseconds(500);
      }
    }
    // No XR frame loop, encoder, capture, or redraw work runs while hidden.
    std::this_thread::sleep_for(std::chrono::milliseconds(visible ? 25 : 250));
  }
  return 0;
}


} // namespace surfxr::dashboard
