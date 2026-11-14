#pragma once
#include "surfxr/latest_frame.hpp"
#include "surfxr/vr_wire.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr {
struct VrStereoFrame {
    std::array<std::shared_ptr<const VideoFrame>,2> eyes;
    std::array<std::shared_ptr<const VideoFrame>,2> focus;
    bool foveated=false;
    // Focus TopLeft is in the configured streaming canvas, which can differ
    // from the game's supersampled dimensions carried in frame metadata.
    std::uint32_t reconstruction_width=0,reconstruction_height=0;
    std::shared_ptr<const VrVideoMetadata> metadata;
    std::uint64_t origin_epoch=0; // epoch correlated to metadata.tracking_frame_id
    // Frame monotonic time when all active streams and their sent-pose identity
    // first formed this immutable pair. Zero means unavailable.
    std::uint64_t ready_monotonic_ns=0;
    // Original Frame monotonic sampling time from the matching sent-pose ledger,
    // not the newest local pose. Zero means unavailable. Submission minus this
    // value is a pose-sample round-trip age, not motion-to-photon latency.
    std::uint64_t pose_sampled_monotonic_ns=0;
    // Original matched tracking query and future target. Never replace these
    // or the image's render pose/FOV with the latest local tracking snapshot.
    // Missing query association stays zero and cannot train motion prediction.
    std::uint64_t pose_query_monotonic_ns=0;
    std::int64_t pose_query_xr_time=0,pose_target_xr_time=0;
};

inline std::array<std::uint32_t,2> vr_reconstruction_extent(const VrStereoFrame& pair) noexcept {
    if(pair.foveated)return {pair.reconstruction_width,pair.reconstruction_height};
    return pair.metadata?std::array<std::uint32_t,2>{pair.metadata->render_width,pair.metadata->render_height}:
        std::array<std::uint32_t,2>{};
}

inline std::array<std::uint64_t,2> vr_decode_time_range(const VrStereoFrame& pair) noexcept {
    std::uint64_t oldest=UINT64_MAX,newest=0;
    const auto include=[&](const auto& frames){for(const auto& frame:frames){
        const auto time=frame?frame->decoded_monotonic_ns:0;
        oldest=std::min(oldest,time);newest=std::max(newest,time);
    }};
    include(pair.eyes);if(pair.foveated)include(pair.focus);
    return {oldest,newest};
}

// Pure validation before any XR/GPU operation. No current pose or epoch may be
// substituted for metadata that travelled with the encoded access unit.
inline bool same_vr_presentation_metadata(const VrVideoMetadata& a,const VrVideoMetadata& b) noexcept {
    if(a.timestamp!=b.timestamp||a.tracking_frame_id!=b.tracking_frame_id||a.surfxr_tracking_timestamp!=b.surfxr_tracking_timestamp||
       a.render_width!=b.render_width||a.render_height!=b.render_height||a.linear_layout!=b.linear_layout||
       a.gaze_atlas.base_width!=b.gaze_atlas.base_width||a.gaze_atlas.base_height!=b.gaze_atlas.base_height||
       a.gaze_atlas.focus_width!=b.gaze_atlas.focus_width||a.gaze_atlas.focus_height!=b.gaze_atlas.focus_height)return false;
    for(unsigned eye=0;eye<2;++eye){
        const auto p=a.eye_poses[eye],q=b.eye_poses[eye];const auto f=a.fov[eye],g=b.fov[eye];
        if(p.position.x!=q.position.x||p.position.y!=q.position.y||p.position.z!=q.position.z||
           p.orientation.x!=q.orientation.x||p.orientation.y!=q.orientation.y||p.orientation.z!=q.orientation.z||p.orientation.w!=q.orientation.w||
           f.up!=g.up||f.down!=g.down||f.left!=g.left||f.right!=g.right||
           a.regions[eye].x!=b.regions[eye].x||a.regions[eye].y!=b.regions[eye].y)return false;
    }
    return true;
}
inline bool valid_vr_video_frame(const std::shared_ptr<const VideoFrame>& f,const VrVideoMetadata& m) noexcept {
    const auto* pixels=f&&f->packed_source?f->packed_source.get():f.get();
    const bool packed=f&&f->packed_source;
    if(!pixels||pixels->packed_source||pixels->stride<std::uint64_t{pixels->width}*4)return false;
    if(pixels->nv12_storage){const auto& s=*pixels->nv12_storage;
        if(!valid_nv12_storage(s)||s.width!=pixels->width||s.height!=pixels->height||!s.data(0)||!s.data(1)||(!m.linear_layout&&!m.gaze_atlas))return false;
    }else if(std::uint64_t{pixels->stride}*pixels->height!=pixels->rgba_size()||!pixels->rgba_data()||pixels->rgba_size()>256ULL*1024*1024)return false;
    if(packed&&((!m.linear_layout&&!m.gaze_atlas)||!f->valid_packed_region()||
       pixels->generation!=f->generation||pixels->source_timestamp!=f->source_timestamp||pixels->vr_metadata!=f->vr_metadata||
       pixels->output_epoch!=f->output_epoch))return false;
    return f&&f->generation&&f->width&&f->height&&f->width<=8192&&f->height<=8192&&
        f->has_source_timestamp&&f->source_timestamp==m.timestamp&&f->stride>=std::uint64_t{f->width}*4&&
        f->vr_metadata&&f->vr_metadata->header_present&&f->vr_metadata->pose_valid&&f->vr_metadata->fov_valid&&
        same_vr_presentation_metadata(*f->vr_metadata,m);
}
inline bool valid_vr_stereo_frame(const VrStereoFrame& pair,std::uint64_t epoch) noexcept {
    if(!epoch||pair.origin_epoch!=epoch||!pair.metadata)return false;
    const auto& m=*pair.metadata;
    if(!m.header_present||!m.pose_valid||!m.fov_valid)return false;
    const auto extent=vr_reconstruction_extent(pair);
    if(pair.foveated&&(!extent[0]||!extent[1]||extent[0]>8192||extent[1]>8192))return false;
    if(m.gaze_atlas&&(!pair.foveated||extent!=std::array<std::uint32_t,2>{m.render_width,m.render_height}||m.linear_layout))return false;
    for(unsigned eye=0;eye<2;++eye){const auto& f=pair.eyes[eye];
        if(!valid_vr_video_frame(f,m))return false;
        if(pair.foveated&&(!valid_vr_video_frame(pair.focus[eye],m)||
           pair.focus[eye]->width>extent[0]||pair.focus[eye]->height>extent[1]))return false;
        if(m.gaze_atlas){const auto a=m.gaze_atlas;const auto& focus=*pair.focus[eye];
            if(!f->packed_region||!focus.packed_region||!f->packed_source||f->packed_source!=focus.packed_source||
               f->packed_source!=pair.eyes[0]->packed_source||f->packed_source->width!=2*a.base_width||
               f->packed_source->height!=a.base_height+a.focus_height||f->width!=a.base_width||f->height!=a.base_height||
               focus.width!=a.focus_width||focus.height!=a.focus_height||f->source_x()!=eye*a.base_width||f->source_y()!=0||
               focus.source_x()!=eye*a.base_width||focus.source_y()!=a.base_height||a.focus_width>extent[0]||a.focus_height>extent[1]||
               m.regions[eye].x<0||m.regions[eye].y<0||std::uint32_t(m.regions[eye].x)>extent[0]-a.focus_width||
               std::uint32_t(m.regions[eye].y)>extent[1]-a.focus_height||
               m.regions[eye].x%2||m.regions[eye].y%2)return false;
        }
        TrackingPose pose;pose.pose=m.eye_poses[eye];pose.flags=OrientationValid|PositionValid;
        if(!tracking_pose_valid(pose))return false;
        const auto v=m.fov[eye];
        for(float tangent:{v.up,v.down,v.left,v.right})
            if(!std::isfinite(tangent)||tangent<=0||tangent>100)return false;
    }
    return pair.foveated||(!pair.focus[0]&&!pair.focus[1]);
}
inline std::array<float,4> vr_eye_fov_radians(const VrFovTangents& t) noexcept {
    return {-std::atan(t.left),std::atan(t.right),std::atan(t.up),-std::atan(t.down)};
}
} // namespace surfxr
