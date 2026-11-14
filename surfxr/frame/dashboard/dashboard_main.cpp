#include "dashboard_runtime.hpp"
using namespace surfxr::dashboard;
int main(int argc, char** argv) {
  try {
    bool show = false, test = false, preview_local = false, preview_gaze=false,preview_surfxr=false;
    std::string preview, preview_state = "idle", preview_page="stream";
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--show") show = true;
      else if (arg == "--self-test") test = true;
      else if (arg == "--preview" && i + 1 < argc) preview = argv[++i];
      else if (arg == "--preview-state" && i + 1 < argc) preview_state = argv[++i];
      else if (arg == "--preview-local") preview_local = true;
      else if(arg=="--preview-page"&&i+1<argc)preview_page=argv[++i];
      else if (arg == "--preview-surfxr") preview_surfxr = true;
      else if (arg == "--preview-gaze") preview_gaze = true;
      else if (arg == "--help") {
        std::cout << "surfxr-dashboard [--show] [--self-test] [--preview image.png] [--preview-state idle|streaming|error] [--preview-local] "
                     "[--preview-page stream|image|devices|diagnostics|hud-content|hud-layout|hud-gaze] [--preview-gaze] [--preview-surfxr]\n";
        return 0;
      } else throw std::runtime_error("Unknown or incomplete dashboard option.");
    }
    if (test) { self_test(); return 0; }
    if (!preview.empty()) {
      if (preview_state != "idle" && preview_state != "streaming" && preview_state != "error")
        throw std::runtime_error("Unsupported dashboard preview state.");
      Canvas canvas(width, height);
      auto snapshot = example(preview_state); snapshot.settings_page = preview_local?SettingsPage::Image:preview_page=="stream"?SettingsPage::Stream:preview_page=="image"?SettingsPage::Image:preview_page=="devices"?SettingsPage::Devices:SettingsPage::Diagnostics;
      if(preview_page=="hud-content")snapshot.diagnostics_page=DiagnosticsPage::Content;
      else if(preview_page=="hud-layout")snapshot.diagnostics_page=DiagnosticsPage::Layout;
      else if(preview_page=="hud-gaze")snapshot.diagnostics_page=DiagnosticsPage::Gaze;
      else if(preview_page!="stream"&&preview_page!="image"&&preview_page!="devices"&&preview_page!="diagnostics")throw std::runtime_error("Unsupported preview page.");
      if(preview_surfxr){snapshot.backend=snapshot.connection.backend="surfxr";snapshot.connection.backend_supported=true;
        snapshot.quality.editable=preview_state=="streaming";snapshot.quality.gaze_editable=preview_state!="streaming";
        snapshot.quality.message="SurfXR video settings sync with PC while connected.";}
      if(preview_gaze){snapshot.quality.gaze_available=true;snapshot.quality.gaze_state="active";
        snapshot.quality.values[3]=1;snapshot.quality.focus_width=768;snapshot.quality.focus_height=832;
        snapshot.quality.eye_width=3072;snapshot.quality.eye_height=3264;
        snapshot.quality.base_width=1024;snapshot.quality.base_height=1088;
        snapshot.quality.center_x={.38,.42};snapshot.quality.center_y={.47,.49};
      }
      render(canvas, snapshot, -1, -1);
      canvas.save(preview);
      return 0;
    }
    return run(show);
  } catch (const std::exception& e) {
    std::cerr << "surfxr-dashboard: " << e.what() << '\n';
    return 1;
  }
}
