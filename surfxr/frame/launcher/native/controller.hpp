#pragma once
#include "connection.hpp"
#include <array>

namespace surfxr::launcher {
class Controller {
 public:
  explicit Controller(Paths paths, std::function<Json()> network_reader = local_network);
  ~Controller();
  Json request(const Json& request);
  Json result(bool ok = true, const std::string& message = "");
  void poll();
  void shutdown();
 private:
  struct Search { std::unique_ptr<Process> child; Json source; std::string host; Time deadline; bool cancelled = false; };
  struct Pending { Json change; int64_t serial = 0; Time deadline; std::string message; };
  Paths paths_;
  std::function<Json()> network_reader_;
  Json network_, active_, progress_, found_ = Json::array();
  std::unique_ptr<Process> child_;
  std::unique_ptr<Process> dashboard_start_;
  Time dashboard_deadline_{};
  std::vector<Search> searches_;
  std::array<Pending,3> pending_;
  Time network_deadline_{}, stop_deadline_{}, mode_deadline_{};
  std::string state_ = "idle", mode_ = "desktop", message_ = "Ready to connect.", host_, pending_mode_, search_mode_, search_message_, stop_error_;
  bool configured_ = false, stop_requested_ = false, preferred_search_ = false;
  int stop_stage_ = 0;
  Json last_exit_, failure_;
  Json steamvr_startup_;
  std::uint64_t startup_requested_ms_=0;
  void update_startup(const Json& progress);
  Json network();
  Json connection_result(bool ok = true, const std::string& message = "");
  Json configure(const Json& request);
  Json start(const std::string& mode, Json resolved = {}, bool scanned = false);
  Json discover(const std::string& mode = "", Json preferred = {});
  void cancel_search();
  void poll_search();
  void launch(Json data, Json effective, const std::string& mode, bool scanned);
  void begin_stop(bool normal, const std::string& error = "");
  void finish_child();
  void poll_dashboard();
  Json preferences(int family, bool ok = true, const std::string& message = "");
  Json change_preference(int family, const Json& request);
  void poll_preferences();
};
int serve(const Paths& paths, std::function<Json()> network_reader = local_network);
Json deployment(const Paths& paths, const std::string& command, const Json& request);
Json verify_release(const fs::path& root);
bool install_runtime(const Paths& paths, const fs::path& build);
void validate_purge(const Paths& paths);
Json configure_input_driver(const Paths& paths,bool enable);
void remove_runtime(const Paths& paths, bool purge = false);
}
