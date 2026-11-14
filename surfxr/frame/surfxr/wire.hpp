#pragma once
#include "surfxr/ports.hpp"
#include "surfxr/tracking.hpp"
#include "surfxr/surfxr_codec.hpp"
#include "surfxr/vr_wire.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace surfxr::transport {
std::uint64_t path_id(const std::string&);
bool is_v21(const std::string& version);
Bytes capabilities(const std::string& host,unsigned width,unsigned height,const std::vector<float>& rates,const std::string& version="20.14.1",bool split_streams=false,bool udp_only=false,bool gaze=false,bool frame_inputs=false,const std::string& client_hostname="",bool desktop=false,bool desktop_tracking=false,bool pose_time=false);
Bytes gaze_feedback(const GazeSample*,std::uint64_t now,std::uint64_t epoch);
Bytes control(std::uint32_t type,bool v21=false);
Bytes tracking(const TrackingFrame&,std::uint64_t timestamp,bool controllers,bool v21=false,bool pose_time=false);
bool views_changed(const TrackingFrame& current,const TrackingFrame& last) noexcept;
Bytes views(const TrackingFrame&,bool v21=false);
Bytes buttons(const TrackingFrame&,bool controllers,bool v21=false,bool frame_inputs=false);
Bytes frame_buttons(const TrackingFrame&,bool enabled);
Bytes frame_skeleton(const TrackingFrame&,unsigned hand,bool controllers,std::uint64_t now);
Bytes frame_profile(unsigned hand);
Bytes playspace(const TrackingFrame&,bool v21=false);
Bytes frame_boundary(const TrackingFrame&);
Bytes profile(unsigned hand,bool v21=false,bool frame_inputs=false);
Bytes battery(std::uint64_t device,float level,bool charging,bool v21=false);
struct StreamConfig {
    unsigned width=0,height=0,port=ports::vr_media,priority_port=0,packet_size=1400,audio_rate=48000;
    float refresh_rate=90;
    bool frame_inputs=false;
    bool frame_skeletons=false;
    bool controller_state=false;
    bool pose_time=false;
    unsigned clock_calibration=0;
    bool boundary_sync=false;
    bool desktop=false;
    bool desktop_multimonitor=false,desktop_capture_control=false;
    std::int32_t desktop_x=0,desktop_y=0,desktop_width=0,desktop_height=0;
    bool session_lifecycle=false;
    bool tcp=false,game_audio=true,microphone=false,full_range=true,video_haptics=false,video_fec=false,image_timestamps=false;
    GazeAtlas gaze_atlas;
};
StreamConfig stream_config(const Bytes&,const std::string& version="20.14.1",bool allow_deprecated_tcp=true);
inline void validate_pose_clock(const StreamConfig& c,bool requested){
    require(!c.pose_time||requested,"surfxr_unsolicited_pose_time");
    require(!requested||(c.pose_time&&c.clock_calibration==2),"surfxr_clock_holdover_required");
}
struct ServerControl {
    unsigned type=0,codec=0;
    std::string reserved;
    Bytes config;
};
ServerControl server_control(const Bytes&,bool v21=false);
struct VideoHeader {
    std::uint64_t timestamp=0,image_timestamp=0;bool idr=false,has_views=false;
    std::array<UiPose,2> poses{};std::array<VrFovTangents,2> fov{};
    std::array<float,2> haptics{};
    std::array<std::array<unsigned,2>,2> crops{};
};
VideoHeader video_header(Reader&,bool v21=false,bool video_haptics=false,bool gaze=false,bool image_timestamps=false);
struct Haptic {unsigned hand=0;std::uint64_t duration_ns=0;float frequency=0,amplitude=0;};
Haptic haptic(const Bytes&,bool v21=false);
}
