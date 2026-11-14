#pragma once
#include "surfxr/remote_settings.hpp"
#include "surfxr/tracking.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace surfxr {
struct VrFovTangents {float up=0,down=0,left=0,right=0;};
struct VrStreamConfig {
    std::uint32_t stream_id=0,eye=0,width=0,height=0;
    bool focus_region=false;
};
// Frame SURFXR v1 atlas: two nonlinear base tiles above two 1:1 focus tiles.
struct GazeAtlas {
    std::uint32_t base_width=0,base_height=0,focus_width=0,focus_height=0;
    explicit operator bool() const noexcept{return base_width!=0;}
};
struct VrConfiguration {
    std::uint32_t codec=0,frame_rate=0,stream_count=0;
    bool foveated=false;
    std::uint32_t render_width=0,render_height=0;
    std::vector<VrStreamConfig> streams;
    bool frame_controller_inputs=false; // SURFXR negotiated physical Frame layout.
    bool stereo_packed=false; // SURFXR: one full-width side-by-side access unit.
    GazeAtlas gaze_atlas;
};
struct VrVideoMetadata {
    std::uint64_t timestamp=0;
    // SURFXR capture identity may differ from the exact uploaded pose timestamp.
    std::uint64_t surfxr_tracking_timestamp=0;
    std::int32_t tracking_frame_id=0;
    bool header_present=false,pose_valid=false,fov_valid=false;
    std::uint64_t header_received_monotonic_ns=0; // primary header, before payload/decoder queues
    std::uint64_t rgba_deadline_monotonic_ns=0;
    std::array<UiPose,2> eye_poses{};
    std::array<VrFovTangents,2> fov{};
    struct EyeRegion {float value=0;std::int32_t x=0,y=0;};
    std::array<EyeRegion,2> regions{};
    std::uint32_t render_width=0,render_height=0;
    bool linear_layout=false;
    GazeAtlas gaze_atlas;
};
enum class SessionCommand { AddMonitor, RemoveMonitor, Keyframe, Restart };
struct SessionControlCommand { SessionCommand type=SessionCommand::Keyframe; };
struct SessionControlStats {std::uint64_t accepted=0,rejected=0,dequeued=0;};
class SessionControlQueue {
public:
    bool add_monitor();
    bool remove_monitor();
    bool force_keyframe();
    bool restart_stream();
    bool pop(SessionControlCommand& command);
    SessionControlStats statistics() const;
private:
    bool push(SessionCommand type);
    mutable std::mutex mutex_;
    std::deque<SessionControlCommand> queue_;
    SessionControlStats stats_;
};
}
