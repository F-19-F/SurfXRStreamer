#include "dashboard_battery.hpp"
#include "surfxr/version.hpp"

namespace surfxr::dashboard {
void render(Canvas& canvas, const Status& s, int hover, int pressed, const std::string& host_entry_notice) {
  cairo_t* cr = canvas.context();
  color(cr, {0.045, 0.06, 0.09}); cairo_paint(cr);
  cairo_pattern_t* gradient = cairo_pattern_create_linear(0, 0, width, height);
  cairo_pattern_add_color_stop_rgba(gradient, 0, 0.10, 0.19, 0.27, 0.8);
  cairo_pattern_add_color_stop_rgba(gradient, 0.8, 0.04, 0.07, 0.11, 0.0);
  cairo_set_source(cr, gradient); cairo_paint(cr); cairo_pattern_destroy(gradient);
  monitor(cr, 60, 39, 0.75, mint);
  const bool surfxr=s.backend=="surfxr";
  text(cr, surfxr::product_title, 128, 73, 36, ink, true);
  text(cr, "STEAM FRAME", 820, 66, 18, muted, true);

  const auto startup_state=surfxr::steamvr_startup_state(s.steamvr_startup_state);
  const auto startup_reason=surfxr::steamvr_startup_reason(s.steamvr_startup_reason);
  const bool connection_failure=s.state=="error"&&!s.failure_code.empty();
  const bool startup_failure=surfxr&&startup_state==surfxr::SteamVrStartupState::Failed&&startup_reason&&s.state!="stopping";
  const bool startup_visible=surfxr&&startup_state&&startup_reason&&s.state!="stopping"&&
      (startup_failure||s.pending_mode=="vr"||(s.mode=="vr"&&s.state!="idle")||
       (s.state=="idle"&&s.configured&&startup_state==surfxr::SteamVrStartupState::NotStarted));
  const bool failure=connection_failure||startup_failure;
  rounded(cr, 56, 110, 840, 138, 24); color(cr, failure ? Color{0.27,0.075,0.085} : Color{0.10, 0.135, 0.18}); cairo_fill(cr);
  const bool live = s.state == "streaming";
  const bool busy = s.state == "starting" || s.state == "stopping" || !s.pending_mode.empty();
  const Color accent = live ? mint : busy ? blue : s.state == "error" || !s.available
                      ? Color{1.0, 0.64, 0.40} : muted;
  std::string label = !s.pending_mode.empty() ? (s.pending_mode == "vr" ? "Switching to SteamVR" : "Switching to desktop") :
                      live ? (s.mode == "vr" ? "Streaming SteamVR" : "Streaming desktop") :
                      s.state == "starting" ? "Connecting to your PC" :
                      s.state == "stopping" ? "Disconnecting" :
                      !s.available ? "Service unavailable" : !s.configured ? "Set up your PC" :
                      s.state == "error" ? "Connection needs attention" : "Ready to connect";
  color(cr, accent); cairo_arc(cr, 88, 149, 7, 0, 6.28318530718); cairo_fill(cr);
  if(startup_visible)label=surfxr::steamvr_startup_title(*startup_state);
  if(connection_failure)label=s.failure_title;
  text(cr, fit(cr,label,30,590), 110, 160, 30, failure ? Color{1.,0.70,0.65} : ink, true);
  std::string detail = s.message;
  if(startup_visible)detail=surfxr::steamvr_startup_detail(*startup_state,*startup_reason);
  if (s.available && !s.configured) detail = "Open PC SurfXR Dashboard, then Find PCs and trust Frame on the PC.";
  if (detail.empty()) detail = live ? "Connected to your PC." : "The PC Streamer must be running.";
  text(cr, fit(cr, connection_failure ? s.failure_detail : detail, failure ? 19 : 17, 780), 80, 195,
       failure ? 19 : 17, failure ? ink : muted, failure);
  const auto& gaze=s.quality;
  const bool gaze_active=live&&s.mode=="vr"&&gaze.gaze_state=="active";
  const auto size=[](unsigned w,unsigned h){return w&&h?std::to_string(w)+"x"+std::to_string(h):std::string("--");};
  const auto gaze_label=gaze_active?"GAZE ACTIVE":gaze.gaze_state=="unavailable"?"GAZE UNAVAILABLE - NORMAL STREAM":
      gaze.values[3]==0?"GAZE OFF":"GAZE ON - WAITING FOR VR";
  const std::string geometry=gaze_active?std::string("  |  Center ")+size(gaze.focus_width,gaze.focus_height)+
      "  |  Base "+size(gaze.base_width,gaze.base_height)+"  |  Full eye "+size(gaze.eye_width,gaze.eye_height):"";
  text(cr,fit(cr,connection_failure?s.failure_action:startup_failure?"Select SteamVR to retry, or continue using Desktop.":std::string(gaze_label)+geometry,failure ? 17 : 19,780),
       80,232,failure ? 17 : 19,failure ? Color{1.,0.80,0.55} : gaze_active?mint:muted,true);

  rounded(cr, 56, 394, 552, height - 436, 24); color(cr, {0.085, 0.115, 0.16}); cairo_fill(cr);
  rounded(cr, 636, 394, 588, height - 436, 24); color(cr, {0.085, 0.115, 0.16}); cairo_fill(cr);
  text(cr, "CONNECTION", 80, 429, 19, muted, true);

  const auto& connection = s.connection;
  const auto& selected = connection.profiles[profile_index(connection.profile)];
  const auto& last = connection.last_hosts[profile_index(connection.profile)];
  const auto shown_host = connection.remember_last_ip && !last.empty() ? last : selected.host;
  text(cr, "PC", 80, 545, 19, muted, true);
  text(cr, fit(cr, shown_host.empty() ? (surfxr&&connection.surfxr_version=="20.14.1"?"Enter SURFXR PC IP":"Automatic discovery") : shown_host, 28, 446), 130, 546, 28, ink, true);
  const auto candidates = host_choices(connection);
  std::string candidate_label = candidates.empty() ? "No nearby PC address" : "Choose PC / link";
  for (size_t i = 0; i != candidates.size(); ++i) if (same_choice(selected, candidates[i]))
    candidate_label = selected.host.empty() ? "Find a PC before connecting" :
        surfxr::i18n::translate("PC / link choices") + " " + std::to_string(i + 1) + "/" + std::to_string(candidates.size());
  if (connection.remember_last_ip && !last.empty())
    candidate_label = "Last successful connection";
  text(cr, fit(cr, candidate_label, 21, 360), 153, 600, 21, muted);
  const std::string local = selected.local_address.empty() ? "Automatic network route" : "Frame: " + selected.local_address;
  std::string link_note = !connection.loaded ? "Loading connection settings..." :
      connection.reason == "discovery_required" ? "Blank PC address: discover before connecting." :
      connection.reason == "host_required" ? "Select a PC address to connect." :
      connection.ready ? "Changes apply on the next connection." :
      "Selected link is currently unavailable.";
  if (connection.discovering) link_note = "Searching Wi-Fi and 6 GHz...";
  else if (connection.ready && !connection.discovery_message.empty()) link_note = connection.discovery_message;
  if (!connection.discovering && connection.remember_last_ip && !last.empty())
    link_note = surfxr&&connection.surfxr_version=="20.14.1"?"Use last connected IP; SURFXR trust is managed on PC.":"Try last connected IP first; discover if unreachable.";
  if(surfxr&&!connection.discovering&&last.empty())link_note=connection.surfxr_version=="20.14.1"?
    "Add and trust this Frame in the PC SURFXR client list.":"Open PC SurfXR Dashboard; connecting starts PC SteamVR.";
  {
    text(cr, fit(cr, local, 18, 496), 80, 634, 18, muted);
    text(cr, fit(cr, link_note, 16, 500), 80, 658, 16, connection.loaded && !connection.ready ? Color{1., .72, .45} : muted);
  }
  text(cr, fit(cr, host_entry_notice.empty() ? "SURFXR uses UDP; PC controls video quality and refresh rate." : host_entry_notice,
      14, 500), 80, height - 56, 14, muted);

  const auto& rows=settings_rows(s.settings_page,s.diagnostics_page);
  constexpr std::array<const char*,9> quality_names{"Potato","Low","Medium","High","Ultra","Godlike","Monster","Rampage","Unstoppable"};
  for(size_t row=0;row<rows.size();++row){
    const auto& item=rows[row];const double y=468+row*68;
    if(row%2==0){rounded(cr,650,y-4,556,62,10);color(cr,{.105,.14,.19});cairo_fill(cr);}
    text(cr,item.label,660,y+21,20,ink,true);
    text(cr,fit(cr,item.note,14,326),660,y+44,14,muted);
    if(!binary(item)){
      std::string value="--";
      if(row_loaded(s,item)){
        const auto v=row_value(s,item);
        if(item.family==SettingFamily::Input)value=std::to_string(static_cast<int>(v))+"%";
        else if(item.id==0)value=v==-2?"Custom":quality_names[static_cast<size_t>(std::clamp(static_cast<int>(v)+1,0,8))];
        else{std::ostringstream formatted;formatted<<v<<(item.id==1?" Hz":" Mbps");value=formatted.str();}
      }
      text(cr,fit(cr,value,21,150),838,y+21,21,mint,true);
    }
  }
  const bool input_page=s.settings_page==SettingsPage::Devices||s.settings_page==SettingsPage::Diagnostics;
  const auto& quality=s.settings_page==SettingsPage::Image?s.image_quality:s.quality;
  std::string note=input_page?s.input_preferences.message:quality.message;
  if(note.empty())note=s.settings_page==SettingsPage::Stream?"Quality / FPS apply after restarting SteamVR.":
    s.settings_page==SettingsPage::Image?"Image processing on this Frame.":
    s.settings_page==SettingsPage::Devices?"Battery source changes the level sent to PC.":
    s.diagnostics_page==DiagnosticsPage::Gaze?"Gaze tracking required. Off keeps the Mini HUD visible.":"Optional overlays for troubleshooting.";
  const bool diagnostics=s.settings_page==SettingsPage::Diagnostics;
  text(cr,fit(cr,note,diagnostics?12:16,535),660,diagnostics?797:777,diagnostics?12:16,muted);

  for (int i = 0; i != static_cast<int>(buttons.size()); ++i) {
    if (!visible(s, i)) continue;
    const auto b = button_rect(i);
    const bool active = enabled(s, i);
    const bool over = active && hover == i;
    const bool down = active && pressed == i && over;
    const int row_index=setting_row_index(i);
    const bool selected_choice=row_index>=0&&binary(rows[row_index])&&row_loaded(s,rows[row_index])&&
      (row_value(s,rows[row_index])!=0)==(i%2!=0);
    const bool selected_profile=(i>=3&&i<6&&connection.loaded&&profile_index(connection.profile)==static_cast<size_t>(i-3))||
      (is_page_button(i)&&page_button(s.settings_page)==i)||
      (is_diagnostics_button(i)&&36+static_cast<int>(s.diagnostics_page)==i)||selected_choice;
    Color bg = selected_profile ? Color{0.12, 0.31, 0.32} : !active ? Color{0.10, 0.125, 0.16} :
               i == 2 ? Color{0.21, 0.14, 0.15} : Color{0.10, 0.24, 0.255};
    if (over) { bg.r += 0.035; bg.g += 0.05; bg.b += 0.05; }
    if (down) { bg.r *= 0.8; bg.g *= 0.8; bg.b *= 0.8; }
    rounded(cr, b.x, b.y, b.w, b.h, i < 3 ? 20 : 12); color(cr, bg); cairo_fill_preserve(cr);
    color(cr, active || selected_profile ? (i == 2 ? Color{0.81, 0.43, 0.42} : mint) : muted, selected_profile || over ? 0.9 : 0.25);
    cairo_set_line_width(cr, over ? 3 : 1.5); cairo_stroke(cr);
    if (i < 3) {
      text(cr, b.title, b.x + 25, b.y + 40, 29, active ? ink : muted, true);
      text(cr, b.subtitle, b.x + 25, b.y + 72, 20, muted);
    } else {
      const double font=row_index>=0?(binary(rows[row_index])?(rows[row_index].family==SettingFamily::Stream&&rows[row_index].id==5?16:18):26):is_diagnostics_button(i)?15:is_page_button(i)?17:i>=27?18:21;
      const char* title=row_index>=0&&binary(rows[row_index])?choice_label(rows[row_index],i%2!=0):
        i==31?(i18n::language_selection()=="auto"?"Language: SteamVR":i18n::language_selection()=="zh-CN"?"Language: 简体中文":"Language: English"):
        i==28?"PC address":i==27?(connection.remember_last_ip?"Remember PC: On":"Remember PC: Off"):b.title;
      const auto title_width=text_width(cr,title,font);
      text(cr, title, b.x + (b.w - title_width) / 2, b.y + (b.h + font * .7) / 2,
           font, active || selected_profile ? ink : muted, true);
    }
  }
  battery_card(cr,s);
}

} // namespace surfxr::dashboard
