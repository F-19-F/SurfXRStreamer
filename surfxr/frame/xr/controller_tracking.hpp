#pragma once
#include "surfxr/tracking.hpp"
#include "surfxr/motion_sync.hpp"

namespace surfxr::xr_detail {
// SURFXR's PC driver extrapolates grip pose using the supplied velocities, as
// does SteamVR after the driver's poseTimeOffset. Feeding a future XR pose
// here predicts twice, especially visibly when the hand changes direction.
inline std::int64_t controller_tracking_time(const TrackingFrame& frame,
        std::int64_t epoch_begin,std::int64_t next_change) noexcept {
    if(!frame.prediction_time_valid)return 0;
    const auto time=frame.query_xr_time;
    // Local display can already select the next Stage epoch while XR now is
    // still in the previous one. Never label that current controller pose
    // with the new epoch or fall back to the predicted head's timestamp.
    return motion_target_in_epoch(time,epoch_begin,next_change)?time:0;
}
}
