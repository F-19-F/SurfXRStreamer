#include "surfxr/version.hpp"
#include "platform.hpp"
#include "surfxr/remote_settings.hpp"
#include <gtk/gtk.h>
#include "surfxr/localization.hpp"
#include <array>
#include <cmath>

namespace surfxr::launcher {
namespace {
struct Preference { const char* label; const char* name; int family; double minimum, maximum, step; GtkWidget* widget = nullptr; };
struct Window {
  Paths paths;
  GtkWidget *window, *message, *profile, *language, *host, *local, *remember, *desktop, *vr, *stop, *find, *save;
  bool updating = false, dirty = false, reload=false;
  guint timer = 0;
  std::string signature;
  Json choices = Json::array(), interfaces = Json::array();
  std::array<Preference,25> preferences{{
    {"VR quality (-1 Potato to 7 Unstoppable)","vr_quality",0,-1,7,1}, {"VR FPS","vr_fps",0,24,144,1}, {"VR bitrate (Mbps)","vr_bitrate_mbps",0,5,1000,10},
    {"Dynamic Foveated Streaming","foveated_streaming",0,0,1,1}, {"Desktop mip filter","desktop_filter",1,0,1,1}, {"VR upscaling","vr_upscaling",1,0,1,1},
    {"VR timewarp (SURFXR)","vr_timewarp",1,0,1,1},
    {"VR sharpening","vr_sharpening_enabled",1,0,1,1}, {"Sharpening strength (%)","vr_sharpening",1,0,100,5},
    {"VR controllers","vr_controllers_enabled",2,0,1,1}, {"Performance HUD","performance_overlay",2,0,1,1},
    {"Gaze debug center","gaze_debug_overlay",2,0,1,1},
    {"Sync safety boundary to PC","boundary_sync_enabled",2,0,1,1},
    {"Sync raw headset battery to PC","sync_raw_battery",2,0,1,1},
    {"Switching HUD","transition_hud_enabled",2,0,1,1},
    {"Mini HUD","mini_hud_enabled",2,0,1,1}, {"Mini HUD: FPS","mini_hud_fps",2,0,1,1},
    {"Mini HUD: latency","mini_hud_latency",2,0,1,1}, {"Mini HUD: frame time","mini_hud_frame_time",2,0,1,1},
    {"Mini HUD: decode time","mini_hud_decode",2,0,1,1}, {"Mini HUD: show only when gazed at","mini_hud_gaze_only",2,0,1,1},
    {"Mini HUD: show gaze target marker","mini_hud_gaze_marker",2,0,1,1},
    {"Mini HUD: horizontal (left to right %)","mini_hud_x",2,0,100,5},
    {"Mini HUD: vertical (top to bottom %)","mini_hud_y",2,0,100,5}, {"Mini HUD: size (%)","mini_hud_scale",2,50,200,10}
  }};
  Json call(Json body) {
    try { return ipc(paths,body); } catch (...) { return object({{"ok",false},{"message","The connection service is unavailable."}}); }
  }
  void notice(const Json& response) { gtk_label_set_text(GTK_LABEL(message),i18n::translate(response["message"].text()).c_str()); }
  static void dirty_changed(GtkWidget*, gpointer pointer) { auto& w = *static_cast<Window*>(pointer); if (!w.updating) w.dirty = true; }
  static void host_changed(GtkComboBox* combo, gpointer pointer) {
    auto& w = *static_cast<Window*>(pointer); if (w.updating) return;
    const auto* id = gtk_combo_box_get_active_id(combo);
    if (id) {
      const auto index = static_cast<size_t>(std::stoul(id)); auto choice = w.choices.at(index);
      auto kind = choice["kind"].text(), local = choice["local_address"].text();
      if (local.empty()) for (size_t i = 0; i < w.interfaces.size(); ++i) if (w.interfaces.at(i)["name"].text() == choice["interface"].text()) local = w.interfaces.at(i)["address"].text();
      w.updating = true;
      if (kind == "lan" || kind == "direct") gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.profile),kind.c_str());
      gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(w.host))),choice["host"].text().c_str());
      gtk_entry_set_text(GTK_ENTRY(w.local),local.c_str()); w.updating = false;
    }
    w.dirty = true;
  }
  static void profile_changed(GtkComboBox* combo, gpointer pointer) {
    auto& w = *static_cast<Window*>(pointer); if (w.updating) return;
    const auto* id = gtk_combo_box_get_active_id(combo); if (!id) return;
    auto result = w.call(object({{"command","config_get"}}));
    if (result["ok"].boolean()) {
      auto endpoint = result["connection_config"]["profiles"][id]; w.updating = true;
      gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(w.host))),endpoint["host"].text().c_str());
      gtk_entry_set_text(GTK_ENTRY(w.local),endpoint["local_address"].text().c_str()); w.updating = false;
    }
    w.dirty = true;
  }
  static void action(GtkButton* button, gpointer pointer) {
    auto& w = *static_cast<Window*>(pointer);
    auto* widget = GTK_WIDGET(button); auto command = widget == w.stop ? "stop" : widget == w.find ? "discover" : "start";
    if (std::string(command) == "start") { try {
      (void)run({"systemctl","--user","start","steamvr.service"},8000); (void)run({"systemctl","--user","start","surfxr-dashboard.service"},8000);
    } catch (...) { gtk_label_set_text(GTK_LABEL(w.message),i18n::translate("SteamVR could not start.").c_str()); return; } }
    auto request = object({{"command",command}}); if (std::string(command) == "start") request.set("mode",widget == w.vr ? "vr" : "desktop");
    w.notice(w.call(request));
  }

  static void save_connection(GtkWidget*, gpointer pointer) {
    auto& w = *static_cast<Window*>(pointer);
    const auto* selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.profile));
    auto* host_entry = gtk_bin_get_child(GTK_BIN(w.host));
    auto body = object({{"command","config_set"},{"profile",selected ? selected : "auto"},{"host",gtk_entry_get_text(GTK_ENTRY(host_entry))},
      {"local_address",gtk_entry_get_text(GTK_ENTRY(w.local))},{"remember_last_ip",bool(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w.remember)))}});


    body.set("backend","surfxr");
    body.set("surfxr",object({{"version","21.0.0-dev14"}}));
    auto response = w.call(body); w.notice(response); if (response["ok"].boolean()) { w.dirty = false; w.signature.clear(); }
  }
  static void preference_changed(GtkWidget* widget, gpointer pointer) {
    auto& w = *static_cast<Window*>(pointer); if (w.updating) return;
    for (const auto& preference : w.preferences) if (preference.widget == widget) {
      constexpr const char* commands[] = {"settings_set","image_quality_set","input_preferences_set"};
      const auto* rate = GTK_IS_COMBO_BOX(widget) ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)) : nullptr;
      const int value = rate ? std::stoi(rate) : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
      w.notice(w.call(object({{"command",commands[preference.family]},{"setting",preference.name},{"value",value}})));
    }
  }
  gboolean refresh() {
    auto response = call(object({{"command","config_get"}}));
    if (!response["ok"].boolean()) { notice(response); for (auto* button : {desktop,vr,stop,find}) gtk_widget_set_sensitive(button,false); return G_SOURCE_CONTINUE; }
    updating = true;
    auto connection = response["connection_config"]; const auto state = response["state"].text(), mode = response["pending_mode"].text(response["mode"].text());
    const bool surfxr = response["backend"].text() == "surfxr";
    gtk_window_set_title(GTK_WINDOW(window),surfxr::product_title);

    interfaces = connection["interfaces"];
    const bool ready = response["configured"].boolean() && connection["effective"]["ready"].boolean();
    for (auto* button : {desktop,vr}) gtk_widget_set_sensitive(button,(!surfxr||button==vr||connection["surfxr"]["version"].text()=="21.0.0-dev14") && ready && state != "stopping" && (state != "starting" && state != "streaming" ? true : mode != (button == vr ? "vr" : "desktop")));
    gtk_widget_set_sensitive(stop,state == "starting" || state == "streaming");
    gtk_widget_set_sensitive(find,(connection["surfxr"]["version"].text()=="21.0.0-dev14") && !connection["discovery"]["running"].boolean());
    notice(response);
    if (connection["discovery"]["running"].boolean()) gtk_label_set_text(GTK_LABEL(message),i18n::translate(connection["discovery"]["message"].text()).c_str());
    auto form = object({{"backend",connection["backend"]},{"surfxr",connection["surfxr"]},{"profile",connection["profile"]},{"profiles",connection["profiles"]},{"candidates",connection["candidates"]},{"remember",connection["remember_last_ip"]}}).dump();
    if (!dirty && form != signature) {
      const auto selected = connection["profile"].text("auto");
      const auto endpoint = connection["profiles"][selected];

      gtk_combo_box_set_active_id(GTK_COMBO_BOX(profile),selected.c_str());
      gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(host)); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(host),"");
      choices = connection["candidates"];
      for (size_t i = 0; i < choices.size(); ++i) {
        auto choice = choices.at(i); auto id = std::to_string(i), label = choice["host"].text()+" ("+choice["kind"].text()+" / "+choice["interface"].text()+")";
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(host),id.c_str(),label.c_str());
      }
      gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(host))),endpoint["host"].text().c_str());
      gtk_entry_set_text(GTK_ENTRY(local),endpoint["local_address"].text().c_str());
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remember),connection["remember_last_ip"].boolean()); signature = form;
    }
    constexpr const char* gets[] = {"settings_get","image_quality_get","input_preferences_get"};
    constexpr const char* groups[] = {"stream_settings","image_quality","input_preferences"};
    for (int family = 0; family < 3; ++family) {
      auto answer = call(object({{"command",gets[family]}})); auto values = answer[groups[family]];
      for (auto& preference : preferences) if (preference.family == family) {
        if (!preference.widget) continue;
        gtk_widget_set_sensitive(preference.widget,answer["ok"].boolean() && (std::string(preference.name)=="foveated_streaming"?values["gaze_editable"].boolean():values["editable"].boolean()));
        double maximum = preference.maximum;
        if (std::string(preference.name) == "vr_quality") maximum = values["max_vr_quality"].number(7);
        if (std::string(preference.name) == "vr_bitrate_mbps") maximum = values["max_vr_bitrate_mbps"].number(200);
        auto value = values[preference.name];
        if (GTK_IS_COMBO_BOX(preference.widget)) {
          auto selected = std::to_string(static_cast<int>(value.number())); gtk_combo_box_set_active_id(GTK_COMBO_BOX(preference.widget),selected.c_str());
        } else {
          gtk_spin_button_set_range(GTK_SPIN_BUTTON(preference.widget),preference.minimum,maximum);
          gtk_spin_button_set_value(GTK_SPIN_BUTTON(preference.widget),value.is(json_type_boolean) ? double(value.boolean()) : value.number());
        }
      }
    }
    updating = false; return G_SOURCE_CONTINUE;
  }
};
GtkWidget* button(GtkWidget* box, const char* label, GCallback callback, Window* w) {
  auto* value = gtk_button_new_with_label(i18n::translate(label).c_str()); gtk_box_pack_start(GTK_BOX(box),value,false,false,0); g_signal_connect(value,"clicked",callback,w); return value;
}
}
static int desktop_window_once(const Paths& paths,bool& reload) {
  i18n::initialize_fonts();
  if (!gtk_init_check(nullptr,nullptr)) throw std::runtime_error("desktop_display_unavailable");
  try { (void)run({"systemctl","--user","start","surfxr-client.service"},8000); } catch (...) {}
  auto* css=gtk_css_provider_new();
  gtk_css_provider_load_from_data(css,"* { font-family: 'SurfXR UI Sans', sans-serif; }",-1,nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  Window w{}; w.paths = paths; w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(w.window),surfxr::product_title); gtk_window_set_default_size(GTK_WINDOW(w.window),620,760);
  auto* scroll = gtk_scrolled_window_new(nullptr,nullptr); gtk_container_add(GTK_CONTAINER(w.window),scroll);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL,8); gtk_container_set_border_width(GTK_CONTAINER(box),18); gtk_container_add(GTK_CONTAINER(scroll),box);
  w.message = gtk_label_new(i18n::translate("Connection setup is required.").c_str()); gtk_label_set_line_wrap(GTK_LABEL(w.message),true); gtk_box_pack_start(GTK_BOX(box),w.message,false,false,0);
  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_box_pack_start(GTK_BOX(box),actions,false,false,0);
  w.desktop = button(actions,"Desktop",G_CALLBACK(Window::action),&w); w.vr = button(actions,"VR",G_CALLBACK(Window::action),&w); w.stop = button(actions,"Disconnect",G_CALLBACK(Window::action),&w); w.find = button(actions,"Find PCs",G_CALLBACK(Window::action),&w);
  w.language=gtk_combo_box_text_new();
  for(auto item:{std::pair{"auto","Language: SteamVR"},std::pair{"zh-CN","简体中文"},std::pair{"en","English"}})
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.language),item.first,i18n::translate(item.second).c_str());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.language),i18n::language_selection().c_str());
  gtk_box_pack_start(GTK_BOX(box),w.language,false,false,0);
  g_signal_connect(w.language,"changed",G_CALLBACK(+[](GtkComboBox* combo,gpointer pointer){
    auto& window=*static_cast<Window*>(pointer);
    if(const char* selected=gtk_combo_box_get_active_id(combo)) {
      try{i18n::select_language(selected);window.reload=true;gtk_widget_destroy(window.window);}
      catch(const std::exception&){gtk_label_set_text(GTK_LABEL(window.message),"Could not save language.");}
    }
  }),&w);
  gtk_box_pack_start(GTK_BOX(box),gtk_label_new(i18n::translate("SurfXR uses one paired connection for Desktop and SteamVR. Video settings sync with the PC.").c_str()),false,false,0);
  w.profile = gtk_combo_box_text_new(); for (auto name : {"auto","lan","direct"}) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.profile),name,i18n::translate(name).c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(w.profile),0); gtk_box_pack_start(GTK_BOX(box),w.profile,false,false,0);
  gtk_box_pack_start(GTK_BOX(box),gtk_label_new(i18n::translate("PC IP (blank = automatic discovery) / Frame source IP (blank = automatic)").c_str()),false,false,0);
  w.host = gtk_combo_box_text_new_with_entry(); w.local = gtk_entry_new();
  for (auto* widget : {w.host,w.local}) gtk_box_pack_start(GTK_BOX(box),widget,false,false,0);
  w.remember = gtk_check_button_new_with_label(i18n::translate("Remember last IP").c_str()); gtk_box_pack_start(GTK_BOX(box),w.remember,false,false,0);
  g_signal_connect(w.local,"changed",G_CALLBACK(Window::dirty_changed),&w);
  g_signal_connect(w.host,"changed",G_CALLBACK(Window::host_changed),&w);
  g_signal_connect(gtk_bin_get_child(GTK_BIN(w.host)),"changed",G_CALLBACK(Window::dirty_changed),&w);
  g_signal_connect(w.profile,"changed",G_CALLBACK(Window::profile_changed),&w);
  g_signal_connect(w.remember,"toggled",G_CALLBACK(Window::dirty_changed),&w);
  w.save = button(box,"Save connection",G_CALLBACK(Window::save_connection),&w);
  for (auto& preference : w.preferences) {
    // Strength remains an IPC/saved preference, as in the existing hidden UI.
    if (std::string(preference.name) == "vr_sharpening") continue;
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_box_pack_start(GTK_BOX(box),row,false,false,0);
    gtk_box_pack_start(GTK_BOX(row),gtk_label_new(i18n::translate(preference.label).c_str()),true,true,0);
    if (std::string(preference.name) == "vr_fps") {
      preference.widget = gtk_combo_box_text_new();
      for (int rate : std::vector<int>{24,30,60,72,80,90,96,100,120,144}) { auto text = std::to_string(rate); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(preference.widget),text.c_str(),text.c_str()); }
    } else preference.widget = gtk_spin_button_new_with_range(preference.minimum,preference.maximum,preference.step);
    gtk_box_pack_end(GTK_BOX(row),preference.widget,false,false,0);
    g_signal_connect(preference.widget,GTK_IS_COMBO_BOX(preference.widget) ? "changed" : "value-changed",G_CALLBACK(Window::preference_changed),&w);
  }
  g_signal_connect(w.window,"destroy",G_CALLBACK(+[](GtkWidget*,gpointer) { gtk_main_quit(); }),nullptr);
  gtk_widget_show_all(w.window); w.refresh(); w.timer = g_timeout_add(500,+[](gpointer p) -> gboolean { return static_cast<Window*>(p)->refresh(); },&w);
  gtk_main(); if (w.timer) g_source_remove(w.timer); reload=w.reload; return 0;
}
int desktop_window(const Paths& paths) {
  bool reload=true;while(reload){reload=false;desktop_window_once(paths,reload);}return 0;
}

}
