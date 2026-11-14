#pragma once
#include "surfxr/latest_frame.hpp"
#include "surfxr/tracking.hpp"

namespace surfxr {
// A copied XR-thread snapshot. Live gaze is diagnostic only; received crops
// remain authoritative for video reconstruction and are never moved by it.
struct GazeHudState {
    bool supported=false,requested=true,active=false;
    GazeSample sample{};
    std::uint64_t current_epoch=0;
    unsigned eye_width=0,eye_height=0,base_width=0,base_height=0;
    unsigned focus_width=0,focus_height=0;
    std::array<float,2> center_x{},center_y{};
};
struct GazeHud {
    // Convert uploaded [-.5,.5] gaze to full-eye UV, matching PC crop math.
    static bool live_point(const GazeHudState&,unsigned eye,std::uint64_t now,std::array<float,2>& point) noexcept;
    // Eye pose is relative to VIEW; FOV is from the same uplink optics sample.
    static bool marker_pose(const GazeHudState&,unsigned eye,std::uint64_t now,const UiPose& eye_pose,
                            const std::array<float,4>& fov,UiPose& output) noexcept;
    static VideoFrame marker_texture();
    static void raster(VideoFrame&,const GazeHudState&,std::uint64_t now,int y_offset=0);
};
} // namespace surfxr
