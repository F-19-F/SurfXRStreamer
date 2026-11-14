#pragma once
#include "surfxr/boundary.hpp"
#include "steamvr_boundary_ipc.hpp"
#include "controller_status.hpp"
#include "headset_battery_status.hpp"
#include <memory>

namespace surfxr::xr_detail {
class SteamVrBoundary {
public:
    SteamVrBoundary();
    ~SteamVrBoundary();
    std::uint32_t sample_count()const;
    float ipd_meters()const;
    std::uint32_t ipd_samples()const;
    std::uint32_t ipd_events()const;
    bool request_refresh(float hz);
    RefreshSnapshot refresh()const;
    ControllerStatus controllers()const;
    HeadsetBatteryStatus headset_battery()const;
    std::shared_ptr<const StageBoundary> snapshot(std::uint64_t epoch,bool ready);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
