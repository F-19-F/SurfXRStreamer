#pragma once
#include "dashboard_settings.hpp"
#include "surfxr/localization.hpp"
#include <pango/pangocairo.h>

namespace surfxr::dashboard {
struct Color { double r, g, b; };
constexpr Color ink{0.94, 0.96, 0.99}, muted{0.60, 0.67, 0.76};
constexpr Color mint{0.35, 0.91, 0.71}, blue{0.36, 0.68, 1.0};
inline void color(cairo_t* cr, Color c, double alpha = 1) { cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha); }
inline void rounded(cairo_t* cr, double x, double y, double w, double h, double r) {
  constexpr double pi = 3.14159265358979323846;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -pi / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, pi / 2);
  cairo_arc(cr, x + r, y + h - r, r, pi / 2, pi);
  cairo_arc(cr, x + r, y + r, r, pi, pi * 1.5);
  cairo_close_path(cr);
}
inline PangoLayout* text_layout(cairo_t* cr,const std::string& value,double size,bool bold=false) {
  surfxr::i18n::initialize_fonts();
  auto* layout=pango_cairo_create_layout(cr);
  auto* font=pango_font_description_new();
  pango_font_description_set_family(font,"SurfXR UI Sans, sans-serif");
  pango_font_description_set_absolute_size(font,size*PANGO_SCALE);
  pango_font_description_set_weight(font,bold?PANGO_WEIGHT_BOLD:PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(layout,font);pango_font_description_free(font);
  const auto translated=surfxr::i18n::translate(value);
  pango_layout_set_text(layout,translated.c_str(),-1);
  return layout;
}
inline double text_width(cairo_t* cr,const std::string& value,double size) {
  auto* layout=text_layout(cr,value,size);int w=0;
  pango_layout_get_pixel_size(layout,&w,nullptr);g_object_unref(layout);return w;
}
inline void text(cairo_t* cr,const std::string& value,double x,double y,double size,Color c=ink,bool bold=false) {
  auto* layout=text_layout(cr,value,size,bold);
  color(cr,c);cairo_move_to(cr,x,y-static_cast<double>(pango_layout_get_baseline(layout))/PANGO_SCALE);
  pango_cairo_show_layout(cr,layout);g_object_unref(layout);
}
inline std::string fit(cairo_t* cr,const std::string& value,double size,double max_width) {
  auto clipped=surfxr::i18n::translate(value);
  if(text_width(cr,clipped,size)<=max_width)return clipped;
  while(!clipped.empty()) {
    const auto end=g_utf8_find_prev_char(clipped.data(),clipped.data()+clipped.size());
    if(!end)break;
    clipped.resize(static_cast<size_t>(end-clipped.data()));
    if(text_width(cr,clipped+"…",size)<=max_width)return clipped+"…";
  }
  return "…";
}

struct Button { double x, y, w, h; const char* title; const char* subtitle; };
constexpr int exit_button = 29;
constexpr std::array<Button, 40> buttons{{
  {56, 272, 368, 94, "Desktop", "Your displays in VR"},
  {456, 272, 368, 94, "SteamVR", "Enter PC VR"},
  {856, 272, 368, 94, "Disconnect", "Return to Steam Frame"},
  {80, 446, 156, 48, "Auto", ""}, {252, 446, 156, 48, "WiFi", ""},
  {424, 446, 156, 48, "6 GHz link", ""},
  {80, 569, 52, 48, "<", ""}, {528, 569, 52, 48, ">", ""},
  {1050, 452, 56, 50, "-", ""}, {1132, 452, 56, 50, "+", ""},
  {1050, 506, 56, 46, "-", ""}, {1132, 506, 56, 46, "+", ""},
  {1050, 558, 56, 46, "-", ""}, {1132, 558, 56, 46, "+", ""},
  {1050, 610, 56, 46, "-", ""}, {1132, 610, 56, 46, "+", ""},
  {660, 402, 126, 38, "Stream", ""}, {796, 402, 126, 38, "Image", ""},
  {}, {}, {}, {}, {}, {}, // Keep semantic button IDs stable; never shown or hit.
  {1050, 662, 56, 46, "-", ""}, {1132, 662, 56, 46, "+", ""},
  {424, 690, 156, 34, "Find PCs", ""},
  {284, 404, 296, 34, "Remember last IP: On", ""},
  {80, 690, 328, 34, "PC address: click to edit", ""},
  {1004, 34, 220, 50, "Exit Dashboard", ""},
  {}, // Reserved button position.
  {470, 34, 328, 50, "Language: SteamVR", ""},
  {}, {}, // Retired standalone timewarp and battery buttons.
  {932, 402, 126, 38, "Devices", ""}, {1068, 402, 128, 38, "Diagnostics", ""},
  {660,746,126,34,"Overlays",""},{796,746,126,34,"HUD content",""},
  {932,746,126,34,"HUD layout",""},{1068,746,128,34,"HUD gaze",""}
}};
inline bool same_choice(const ConnectionChoice& a, const ConnectionChoice& b) {
  return a.host == b.host && a.local_address == b.local_address;
}
inline std::vector<ConnectionChoice> host_choices(const ConnectionConfig& c) {
  std::vector<ConnectionChoice> choices{{"", "", "", ""}};
  const auto& current = c.profiles[profile_index(c.profile)];
  if (!current.host.empty()) choices.push_back(current);
  for (const auto& item : c.candidates) {
    if (c.profile != "auto" && item.kind != c.profile && item.kind != "unknown" && !item.kind.empty()) continue;
    if (std::none_of(choices.begin(), choices.end(), [&](const auto& previous) { return same_choice(previous, item); }))
      choices.push_back(item);
  }
  return choices;
}
inline double next_value(const QualitySettings& q, size_t row, bool plus) {
  const auto& choices = q.choices[row];
  if (plus) for (double value : choices) { if (value > q.values[row] + .001) return value; }
  if (!plus) for (auto value = choices.rbegin(); value != choices.rend(); ++value) {
    if (*value < q.values[row] - .001) return *value;
  }
  return q.values[row];
}
inline Button button_rect(int index){
  auto b=buttons.at(static_cast<size_t>(index));
  const int row=setting_row_index(index);
  if(row>=0){b.y=468+row*68;b.h=52;b.x=index%2?1104:1004;b.w=92;}
  return b;
}
inline bool visible(const Status& s, int index) {
  if(index<0||index>=static_cast<int>(buttons.size()))return false;
  if((index>=18&&index<=25)||index==30||index==32||index==33)return false;
  if(index==31)return true;
  if(is_diagnostics_button(index))return s.settings_page==SettingsPage::Diagnostics;
  const int row=setting_row_index(index);
  return row<0||static_cast<size_t>(row)<settings_rows(s.settings_page,s.diagnostics_page).size();
}
inline bool enabled(const Status& s, int index) {
  if(!visible(s,index))return false;
  if(index==exit_button||index==31)return true;
  if(is_page_button(index))return index!=page_button(s.settings_page);
  if(is_diagnostics_button(index))return index!=36+static_cast<int>(s.diagnostics_page);
  if(index<3)return index==2?s.can_stop():s.can_select(index==1?"vr":"desktop");
  if(!s.available||s.state=="stopping")return false;
  if(index==26)return s.connection.surfxr_version=="21.0.0-dev14"&&!s.connection.discovering;
  if(index==27||index==28)return s.connection.loaded;
  if(index<6)return s.connection.loaded&&profile_index(s.connection.profile)!=static_cast<size_t>(index-3);
  if(index<8){const auto choices=host_choices(s.connection);return s.connection.loaded&&!choices.empty()&&
    (choices.size()>1||!same_choice(choices.front(),s.connection.profiles[profile_index(s.connection.profile)]));}
  const auto row=settings_rows(s.settings_page,s.diagnostics_page).at(static_cast<size_t>(setting_row_index(index)));
  if(!row_loaded(s,row))return false;
  if(row.family==SettingFamily::Input)return s.input_preferences.editable&&input_value(s,row.id)!=next_input_value(s,row.id,index%2!=0);
  const auto& q=row.family==SettingFamily::Image?s.image_quality:s.quality;
  const bool editable=row.family==SettingFamily::Stream&&row.id==3?q.gaze_editable:q.editable;
  return editable&&std::abs(next_value(q,row.id,index%2!=0)-q.values[row.id])>.001;
}
inline Status activate(const std::string& directory, const Status& status, int index) {
  if (!enabled(status, index)) return status;
  if(is_page_button(index)){auto updated=status;updated.settings_page=index==16?SettingsPage::Stream:index==17?SettingsPage::Image:index==34?SettingsPage::Devices:SettingsPage::Diagnostics;return updated;}
  if(is_diagnostics_button(index)){auto updated=status;updated.diagnostics_page=static_cast<DiagnosticsPage>(index-36);return updated;}
  if(index==31){const auto selected=i18n::language_selection();i18n::select_language(selected=="auto"?"zh-CN":selected=="zh-CN"?"en":"auto");return status;}
  if (index == exit_button) { stop_requested = 1; return status; }
  if (index == 28) return status; // The overlay owns SteamVR keyboard events.
  if (index == 26) return merge_status(request(directory, "discover"), status);
  if (index == 27) return merge_status(request_payload(directory,
      std::string("{\"command\":\"config_set\",\"profile\":\"") + status.connection.profile +
      "\",\"remember_last_ip\":" + (status.connection.remember_last_ip ? "false" : "true") + "}\n"), status);
  if (index < 3) return merge_status(request(directory, index == 2 ? "stop" : "start", index == 1 ? "vr" : "desktop"), status);
  if (index < 6) {
    const auto profile = static_cast<size_t>(index - 3);
    return merge_status(save_connection(directory, profile_names[profile], status.connection.profiles[profile]), status);
  }
  if (index < 8) {
    const auto& config = status.connection;
    const auto choices = host_choices(config);
    const auto& current = config.profiles[profile_index(config.profile)];
    const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto& item) { return same_choice(current, item); });
    size_t next = index == 6 ? choices.size() - 1 : 0;
    if (found != choices.end()) {
      const auto selected = static_cast<size_t>(found - choices.begin());
      next = (selected + (index == 6 ? choices.size() - 1 : 1)) % choices.size();
    }
    return merge_status(save_connection(directory, config.profile.c_str(), choices[next]), status);
  }
  const auto row=settings_rows(status.settings_page,status.diagnostics_page).at(static_cast<size_t>(setting_row_index(index)));
  if(row.family==SettingFamily::Input)return merge_status(save_input_preference(directory,next_input_value(status,row.id,index%2!=0),row.id),status);
  const bool local=row.family==SettingFamily::Image;
  const auto& quality=local?status.image_quality:status.quality;
  return merge_status(save_quality(directory,row.id,next_value(quality,row.id,index%2!=0),local),status);
}
inline int hit(const Status& s, double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return -1;
  for (int i = 0; i != static_cast<int>(buttons.size()); ++i) {
    if (!visible(s, i)) continue;
    const auto b = button_rect(i);
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
  }
  return -1;
}

class Canvas {
 public:
  Canvas(int w, int h) : w_(w), h_(h), surface_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h)),
                        cr_(cairo_create(surface_)), rgba_(static_cast<size_t>(w) * h * 4) {
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS || cairo_status(cr_) != CAIRO_STATUS_SUCCESS)
      throw std::runtime_error("Cannot create the dashboard image.");
  }
  ~Canvas() { cairo_destroy(cr_); cairo_surface_destroy(surface_); }
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  cairo_t* context() const { return cr_; }
  void* pixels() {
    cairo_surface_flush(surface_);
    const auto* src = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    for (int y = 0; y != h_; ++y) for (int x = 0; x != w_; ++x) {
      uint32_t argb = 0;
      std::memcpy(&argb, src + y * stride + x * 4, 4);
      const size_t p = (static_cast<size_t>(y) * w_ + x) * 4;
      rgba_[p] = static_cast<uint8_t>(argb >> 16);
      rgba_[p + 1] = static_cast<uint8_t>(argb >> 8);
      rgba_[p + 2] = static_cast<uint8_t>(argb);
      rgba_[p + 3] = static_cast<uint8_t>(argb >> 24);
    }
    return rgba_.data();
  }
  void save(const std::string& path) {
    if (cairo_surface_write_to_png(surface_, path.c_str()) != CAIRO_STATUS_SUCCESS)
      throw std::runtime_error("Cannot save the dashboard preview.");
  }
 private:
  int w_, h_;
  cairo_surface_t* surface_;
  cairo_t* cr_;
  std::vector<uint8_t> rgba_;
};

inline void monitor(cairo_t* cr, double x, double y, double scale, Color tint) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, scale, scale);
  color(cr, tint);
  cairo_set_line_width(cr, 4);
  rounded(cr, 0, 0, 58, 38, 7); cairo_stroke(cr);
  cairo_move_to(cr, 29, 39); cairo_line_to(cr, 29, 51);
  cairo_move_to(cr, 16, 51); cairo_line_to(cr, 42, 51); cairo_stroke(cr);
  cairo_restore(cr);
}


void render(Canvas&,const Status&,int,int,const std::string& host_entry_notice={});
}
