#pragma once
#include "surfxr/latest_frame.hpp"
#include "surfxr/performance_overlay.hpp"
#include "surfxr/bandwidth_overlay.hpp"
#include "surfxr/gaze_hud.hpp"
#include "surfxr/surfxr_performance.hpp"

namespace surfxr {
// VIEW-space panel shared by both eyes. The bottom maps compare live gaze
// with each video's received crop; it owns no interactive input.
struct PerformanceHud {
    static constexpr unsigned width=720,height=559;
    static constexpr float width_m=.54f,height_m=.41925f,y_m=-.41f,z_m=-1.2f;
    static VideoFrame raster(const PerformanceOverlay& values,std::uint64_t now,std::uint64_t generation,
                             const BandwidthOverlay::Summary& bandwidth={},const GazeHudState& gaze={},
                             const SurfxrPerformance::Snapshot& surfxr_timing={});
};
} // namespace surfxr
