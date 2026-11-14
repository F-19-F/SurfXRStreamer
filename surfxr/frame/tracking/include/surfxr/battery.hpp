#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace surfxr {
struct DeviceBattery {
    int percent=-1; // -1 is unknown; 0 is a real empty battery, 100 a real full one.
    bool charging=false;
    bool charging_known=false;
};
struct BatterySnapshot {
    DeviceBattery hmd;
    std::array<DeviceBattery,2> controllers{}; // physical left, right; never enumerate-order
    std::uint64_t hmd_sampled_monotonic_ns=0,controllers_sampled_monotonic_ns=0;
};
std::uint64_t battery_monotonic_ns() noexcept;
DeviceBattery sanitize_battery(DeviceBattery value) noexcept;
// Pure mapping of XR_EXT_interaction_profile_battery_state_display flags.
// Call the typed wrapper in openxr_battery.hpp so an invalid level is not read.
DeviceBattery battery_from_xr_state(bool profile_active,std::uint64_t flags,float level) noexcept;
// Bounded, read-only sysfs inspection. A single unambiguous system battery is
// required. USB/charger online state is never substituted for battery status.
DeviceBattery read_headset_battery(const std::string& root="/sys/class/power_supply") noexcept;
// Expire observations independently: HMD <=30s, controllers <=5s. Future/zero
// timestamps and malformed percentages are unknown, not full or empty.
BatterySnapshot fresh_battery_snapshot(BatterySnapshot value,std::uint64_t now_ns) noexcept;

class BatteryMonitor {
public:
    // Immediately starts one worker, then polls at the selected interval.
    // The worker accesses sysfs only: no XR/OpenVR/session lifecycle or device writes.
    explicit BatteryMonitor(std::string power_supply_root="/sys/class/power_supply",
                            std::chrono::milliseconds interval=std::chrono::seconds(5));
    ~BatteryMonitor();
    BatteryMonitor(const BatteryMonitor&)=delete;
    BatteryMonitor& operator=(const BatteryMonitor&)=delete;
    // XR owner publishes per-role results after its low-frequency query.
    // Unknown results explicitly replace old observations; no last-known carry.
    void publish_controllers(const std::array<DeviceBattery,2>& controllers,std::uint64_t now_ns=0);
    // Prefer the local runtime's display percentage for 5s, then fall back to
    // sysfs. Preserve the bridge sample time; repeated reads cannot renew it.
    void publish_headset_runtime(DeviceBattery value,std::uint64_t sampled_ns);
    // Raw mode never substitutes the runtime's adjusted display percentage.
    void set_sync_raw_battery(bool enabled);
    DeviceBattery raw_headset(std::uint64_t now_ns=0) const;
    BatterySnapshot snapshot(std::uint64_t now_ns=0) const;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
