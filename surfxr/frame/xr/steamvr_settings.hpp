#pragma once
#include "steamvr_boundary_ipc.hpp"
#include <chrono>

namespace surfxr::xr_detail {
// Used only by the isolated OpenVR worker, never the rendering process.
class SteamVrSettings {
public:
    void request(RefreshRequest value);
    bool poll(RefreshSnapshot& value);
private:
    RefreshRequest request_;
    RefreshSnapshot snapshot_;
    unsigned attempts_=0;
    std::chrono::steady_clock::time_point next_{};
};
}
