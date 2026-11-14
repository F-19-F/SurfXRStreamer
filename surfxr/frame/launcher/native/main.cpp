#include "controller.hpp"
#include <csignal>
#include <iostream>
#include <regex>
#include <unistd.h>

namespace {
using namespace surfxr::launcher;
std::string input_request() {
  std::string text; char c; while (std::cin.get(c)) { text += c; if (text.size() > 8192) throw std::runtime_error("deployment_request_too_large"); }
  return text.empty() ? "{}" : text;
}
}
int main(int argc, char** argv) {
  using namespace surfxr::launcher;
  ::signal(SIGPIPE,SIG_IGN);
  try {
    auto paths = Paths::user(); const auto command = argc > 1 ? std::string(argv[1]) : "desktop";
    if (command == "--help") {
      std::cout << "surfxr-client: daemon | desktop | window | status | start | stop | config_get | config_set | discover | settings_get/set | image_quality_get/set | input_preferences_get/set\n"
                   "Deployment: --deploy inspect|stop|deploy|install|uninstall|purge|verify-release|start|input-driver-register|input-driver-unregister (JSON on stdin).\n"
                   "CLI: --mode desktop|vr --profile auto|lan|direct --backend surfxr --surfxr-version 21.0.0-dev14 --surfxr-desktop-port 19950 --host IPV4 --local-address IPV4 --remember-last-ip on|off --setting NAME --value INTEGER --viewer PATH.\n"; return 0;
    }
    if (command == "--deploy") {
      if (argc != 3) throw std::runtime_error("deployment_command_required");
      const std::string action = argv[2];
      auto request = Json::parse(input_request());
      if (!request.is(json_type_object)) throw std::runtime_error("invalid_deployment_request");
      // Uninstall can unlink this running executable, including its release cache.
      // Resolve its staging path while /proc/self/exe still has a live target.
      const auto staging = executable().parent_path().parent_path();
      if (action == "deploy" || action == "install" || action == "start") {
        const auto mode = request["mode"].text(), link = request["link"].text();
        if ((mode != "desktop" && mode != "vr") || (link != "lan" && link != "direct")) throw std::runtime_error("invalid_deployment_request");
      }
      auto response = deployment(paths,action,request); if (!response.has("ok")) response.set("ok",true);
      if (action == "install" || action == "uninstall" || action == "purge") {
        if (std::regex_match(staging.string(),std::regex("/home/[A-Za-z0-9_.-]+/\\.cache/surfxr-client/releases/[a-f0-9]{32}"))) fs::remove_all(staging);
      }
      std::cout << response.dump() << '\n'; return response["ok"].boolean() ? 0 : 1;
    }
    auto body = object({{"command",command}});
    for (int i = 2; i < argc; i += 2) {
      if (i+1 >= argc) throw std::runtime_error("option_value_required");
      const std::string key = argv[i], value = argv[i+1];
      if (key == "--viewer") paths.viewer = fs::absolute(value);
      else if (key == "--mode" || key == "--profile" || key == "--host" || key == "--setting" || key == "--backend") body.set(key.substr(2),value);
      else if (key == "--surfxr-version" && value=="21.0.0-dev14") { auto surfxr = body.has("surfxr") ? body["surfxr"] : Json::object(); surfxr.set("version",value); body.set("surfxr",surfxr); }
      else if (key == "--surfxr-desktop-port" && std::regex_match(value,std::regex("[0-9]{1,5}"))) { auto surfxr=body.has("surfxr")?body["surfxr"]:Json::object();surfxr.set("desktop_port",std::stoi(value));body.set("surfxr",surfxr); }
      else if (key == "--local-address") body.set("local_address",value);
      else if (key == "--remember-last-ip" && (value == "on" || value == "off")) body.set("remember_last_ip",value == "on");
      else if (key == "--value" && std::regex_match(value,std::regex("-?[0-9]{1,6}"))) body.set("value",std::stoi(value));
      else throw std::runtime_error("invalid_option");
    }
    if (command == "daemon") return serve(paths);
    if (command == "desktop") {
      try { if (run({"systemctl","--user","is-active","--quiet","steamvr.service"},3000).code == 0 && fs::is_regular_file(paths.app/"bin/surfxr-dashboard")) {
        (void)run({"systemctl","--user","start","surfxr-dashboard.service"},8000);
        auto dashboard = (paths.app/"bin/surfxr-dashboard").string(); ::execl(dashboard.c_str(),dashboard.c_str(),"--show",nullptr);
      } } catch (...) {}
      return desktop_window(paths);
    }
    if (command == "window") return desktop_window(paths);
    auto response = ipc(paths,body); std::cout << response.dump() << '\n'; return response["ok"].boolean() ? 0 : 1;
  } catch (const std::exception& error) {
    // No raw exception, argv, account, network reply or service stderr escapes.
    const std::string reason = error.what();
    const auto code = std::regex_match(reason,std::regex("[a-z_]{1,128}")) ? reason : "frame_operation_failed";
    std::cout << object({{"ok",false},{"state","error"},{"error",code},{"message","Frame operation failed. Check configuration, SSH access and the connection service."}}).dump() << '\n'; return 1;
  }
}
