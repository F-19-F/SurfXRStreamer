#pragma once
#include <vector>

namespace surfxr::xr_detail {
// Read-only background probe, before OpenXR creates its SteamVR client.
// Empty means unavailable; never invent a display mode or start a runtime.
std::vector<float> steamvr_refresh_rates();
}
