#pragma once
#include "dashboard_model.hpp"

namespace surfxr::dashboard {
enum class SettingFamily { Stream, Image, Input };
struct SettingRow { const char* label; SettingFamily family; unsigned id; const char* note; };
inline const std::vector<SettingRow>& settings_rows(SettingsPage page,DiagnosticsPage diagnostics=DiagnosticsPage::Overlays) {
  using F=SettingFamily;
  static const std::array<std::vector<SettingRow>,4> rows{{
    {{"Quality",F::Stream,0,"Render resolution"},{"Frame rate",F::Stream,1,"Frames per second"},
      {"Bitrate limit",F::Stream,2,"Video bandwidth"},{"Dynamic Foveated Streaming",F::Stream,3,"Applies on next connection"}},
    {{"Desktop filter",F::Image,0,"Reduce shimmer on distant text"},
      {"Spatial upscaling",F::Image,1,"Enlarge the VR image"},{"Timewarp",F::Image,4,"Correct rotation between frames"}},
    {{"VR controllers",F::Input,0,"Forward controller input to PC"},
      {"Safety boundary",F::Input,3,"Sync Frame boundary to PC"},{"Battery source",F::Input,4,"Battery level reported to PC"},
      {"Cursor priority",F::Stream,5,"When mouse and controller both move"}},
    {{"Performance HUD",F::Input,1,"Show frame timing in VR"},
      {"Gaze debug",F::Input,2,"Show the gaze center in each eye"},
      {"Mini HUD",F::Input,5,"Compact FPS and latency display"},
      {"Switching HUD",F::Input,15,"Small notice while desktop / VR switches"}},
  }};
  static const std::array<std::vector<SettingRow>,3> hud_rows{{
    {{"Frame rate",F::Input,6,"New video frames presented per second"},{"Latency",F::Input,7,"Tracking upload to XR submit (estimate)"},
      {"Frame time",F::Input,8,"Average new-video frame interval"},{"Decode time",F::Input,9,"Receive to decoder output"}},
    {{"Horizontal",F::Input,12,"0% left / 100% right"},{"Vertical",F::Input,13,"0% top / 100% bottom"},
      {"Size",F::Input,14,"50% small / 200% large"}},
    {{"Gaze to show",F::Input,10,"Look at the HUD area to reveal it"},
      {"Target marker",F::Input,11,"Mark the area while HUD is hidden"}},
  }};
  if(page==SettingsPage::Diagnostics&&diagnostics!=DiagnosticsPage::Overlays)
    return hud_rows.at(static_cast<size_t>(diagnostics)-1);
  return rows.at(static_cast<size_t>(page));
}
inline int input_value(const Status& s,unsigned id){
  return input_preference_value(s.input_preferences,static_cast<InputPreferenceId>(id));
}
inline int next_input_value(const Status& s,unsigned id,bool plus){
  const auto* spec=input_preference_spec(static_cast<InputPreferenceId>(id));
  if(!spec)return input_value(s,id);
  return spec->boolean?int(plus):std::clamp(input_value(s,id)+(plus?5:-5),spec->minimum,spec->maximum);
}
inline bool binary(const SettingRow& row){
  if(row.family==SettingFamily::Input){const auto* spec=input_preference_spec(static_cast<InputPreferenceId>(row.id));return spec&&spec->boolean;}
  return row.family==SettingFamily::Image||row.id==3||row.id==5;
}
inline bool row_loaded(const Status& s,const SettingRow& row){
  return row.family==SettingFamily::Input?s.input_preferences.loaded:row.family==SettingFamily::Image?s.image_quality.loaded:s.quality.loaded;
}
inline double row_value(const Status& s,const SettingRow& row){
  return row.family==SettingFamily::Input?input_value(s,row.id):(row.family==SettingFamily::Image?s.image_quality:s.quality).values[row.id];
}
inline int setting_row_index(int button){return button>=8&&button<16?(button-8)/2:-1;}
inline int page_button(SettingsPage page){return page==SettingsPage::Stream?16:page==SettingsPage::Image?17:page==SettingsPage::Devices?34:35;}
inline bool is_page_button(int button){return button==16||button==17||button==34||button==35;}
inline bool is_diagnostics_button(int button){return button>=36&&button<=39;}
inline const char* choice_label(const SettingRow& row,bool plus){
  if(row.family==SettingFamily::Stream&&row.id==5)return plus?"PC mouse":"VR hand";
  return row.family==SettingFamily::Input&&row.id==4?(plus?"Actual":"Display"):(plus?"On":"Off");
}
}
