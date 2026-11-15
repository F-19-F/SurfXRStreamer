#include "network_monitor.hpp"
#include <iostream>

using namespace surfxr::launcher;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
  try {
    std::promise<Json> completion;
    int probes = 0;
    NetworkMonitor monitor([&] { ++probes; return completion.get_future(); });
    auto now = Clock::now();
    monitor.poll(now);
    check(probes == 1, "first_probe");
    // Simulate an indefinitely held RTNL lock without sleeping in the test.
    for (int n = 0; n < 100; ++n) monitor.poll(now + std::chrono::seconds(n));
    check(probes == 1 && monitor.snapshot()["interfaces"].size() == 0, "pending_probe_never_blocks_or_overlaps");
    const auto valid = parse_network(Json::parse(R"([{"ifname":"ap","flags":["UP"],"addr_info":[{"family":"inet","local":"192.168.60.2","prefixlen":24}]}])"),
      Json::array(), Json::array(), "Interface ap\n type AP\n channel 5 (5975 MHz)\n");
    completion.set_value(valid);
    monitor.poll(now);
    check(monitor.snapshot().dump() == valid.dump(), "completed_snapshot");
    completion = std::promise<Json>();
    monitor.poll(now + std::chrono::seconds(4));
    check(probes == 1, "normal_refresh_cadence");
    now += std::chrono::seconds(5);
    monitor.poll(now);
    check(probes == 2 && monitor.snapshot().dump() == valid.dump(), "refresh_keeps_valid_snapshot");
    completion.set_exception(std::make_exception_ptr(std::runtime_error("network_probe_failed")));
    monitor.poll(now);
    check(monitor.snapshot().dump() == valid.dump(), "failure_preserves_direct_interface_kind");
    completion = std::promise<Json>();
    now += std::chrono::seconds(1);
    monitor.poll(now);
    check(probes == 3, "failed_probe_retries_in_background");
    completion.set_value(parse_network(Json::array(), Json::array(), Json::array(), ""));
    monitor.poll(now);
    check(monitor.snapshot()["interfaces"].size() == 0, "successful_empty_probe_removes_departed_interfaces");
    std::cout << "Network snapshots preserve valid state through delayed/failed probes and accept real removals.\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
