#pragma once
#include "surfxr/vr_ui.hpp"

namespace surfxr::ui_detail {
// Every category fits within the fixed body; no scrolling past unrelated controls.
inline constexpr float settings_top=.19f,settings_bottom=.79f,settings_content_bottom=settings_bottom;
inline constexpr float monitors_x=.025f,monitors_y=.50f,monitors_width=.445f,controllers_height=.12f;
inline constexpr float system_capture_y=.35f,scroll_x=.985f;
inline constexpr float tab_x=.20f,tab_step=.125f,tab_width=.115f;
inline constexpr std::array<const char*,4> tab_names{"STREAM","IMAGE","DEVICES","DIAGNOSTICS"};
inline constexpr float diagnostics_tab_y=.68f,diagnostics_tab_step=.245f,diagnostics_tab_width=.225f,diagnostics_tab_height=.085f;
inline constexpr std::array<const char*,4> diagnostics_tab_names{"OVERLAYS","HUD CONTENT","HUD LAYOUT","HUD GAZE"};
struct PreferenceRow { UiSettingsPage page;InputPreferenceId id;float x,y;const char* label;const char* note;UiDiagnosticsPage diagnostics=UiDiagnosticsPage::Overlays; };
inline constexpr std::array<PreferenceRow,16> preference_rows{{
  {UiSettingsPage::Devices,InputPreferenceId::VrControllersEnabled,.025f,.20f,"VR CONTROLLERS","FORWARD CONTROLLER INPUT TO PC"},
  {UiSettingsPage::Devices,InputPreferenceId::BoundarySyncEnabled,.515f,.20f,"SAFETY BOUNDARY","SYNC FRAME BOUNDARY TO PC"},
  {UiSettingsPage::Devices,InputPreferenceId::SyncRawBattery,.515f,.35f,"BATTERY SOURCE","BATTERY LEVEL REPORTED TO PC"},
  {UiSettingsPage::Diagnostics,InputPreferenceId::PerformanceOverlay,.025f,.20f,"PERFORMANCE HUD","SHOW FRAME TIMING IN VR"},
  {UiSettingsPage::Diagnostics,InputPreferenceId::GazeDebugOverlay,.515f,.20f,"GAZE DEBUG","SHOW THE GAZE CENTER IN EACH EYE"},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudEnabled,.025f,.35f,"MINI HUD","COMPACT FPS AND LATENCY DISPLAY"},
  {UiSettingsPage::Diagnostics,InputPreferenceId::TransitionHudEnabled,.515f,.35f,"SWITCHING HUD","SHOW STATUS WHILE DESKTOP / VR SWITCHES"},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudFps,.025f,.20f,"FRAME RATE","NEW VIDEO FRAMES PRESENTED PER SECOND",UiDiagnosticsPage::Content},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudLatency,.515f,.20f,"LATENCY","TRACKING UPLOAD TO XR SUBMIT ESTIMATE",UiDiagnosticsPage::Content},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudFrameTime,.025f,.35f,"FRAME TIME","AVERAGE NEW-VIDEO FRAME INTERVAL",UiDiagnosticsPage::Content},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudDecode,.515f,.35f,"DECODE TIME","RECEIVE TO DECODER OUTPUT",UiDiagnosticsPage::Content},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudX,.025f,.20f,"HORIZONTAL","0% LEFT / 100% RIGHT",UiDiagnosticsPage::Layout},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudY,.515f,.20f,"VERTICAL","0% TOP / 100% BOTTOM",UiDiagnosticsPage::Layout},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudScale,.025f,.35f,"SIZE","50% SMALL / 200% LARGE",UiDiagnosticsPage::Layout},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudGazeOnly,.025f,.20f,"GAZE TO SHOW","LOOK AT THE HUD AREA TO REVEAL IT",UiDiagnosticsPage::Gaze},
  {UiSettingsPage::Diagnostics,InputPreferenceId::MiniHudGazeMarker,.515f,.20f,"TARGET MARKER","MARK THE AREA WHILE HUD IS HIDDEN",UiDiagnosticsPage::Gaze}
}};
inline bool preference_visible(const PreferenceRow& row,UiSettingsPage page,UiDiagnosticsPage diagnostics){
  return row.page==page&&(page!=UiSettingsPage::Diagnostics||row.diagnostics==diagnostics);
}
inline bool setting_visible(UiSettingsPage page,SettingId id){
  const bool mic=id==SettingId::MicrophoneEnabled||id==SettingId::MicrophoneVolume||id==SettingId::DesktopPointerPriority;
  return mic?page==UiSettingsPage::Devices:page==UiSettingsPage::Stream;
}
}
