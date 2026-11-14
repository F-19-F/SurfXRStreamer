#pragma once
#include "surfxr/tracking.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace surfxr {
struct TestHeadMotionTelemetry {
    bool enabled=false;
    std::uint64_t samples=0,sent_packets=0,feedback_frames=0,matched_feedback_frames=0;
    double elapsed_seconds=0,yaw_degrees=0,pitch_degrees=0;
    double feedback_rotation_degrees=0,feedback_rotation_max_degrees=0,feedback_error_degrees=0;
    std::int32_t feedback_tracking_id=0;
};
// Explicit diagnostics only. The normal tracking path never constructs an
// enabled instance. A fixed anchor makes unattended A/B motion repeatable;
// controllers, visibility, validity and real pose clocks remain untouched.
class TestHeadMotion {
public:
    explicit TestHeadMotion(bool enabled=false){stats_.enabled=enabled;}
    bool apply(TrackingFrame& frame){
        if(!stats_.enabled||frame.origin!=TrackingOrigin::Stage||!frame.stage_locatable||
           !frame.optics_valid||!tracking_pose_valid(frame.head)||!frame.sampled_monotonic_ns)return false;
        for(const auto& eye:frame.eye_poses)if(!valid(eye.orientation)||!std::isfinite(eye.position.x)||
            !std::isfinite(eye.position.y)||!std::isfinite(eye.position.z))return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!started_||epoch_!=frame.origin_epoch||frame.sampled_monotonic_ns<start_ns_){
            started_=true;epoch_=frame.origin_epoch;start_ns_=frame.sampled_monotonic_ns;last_sample_ns_=0;
            anchor_=frame.head.pose;anchor_.orientation=normalized(anchor_.orientation);
            const auto inverse=conjugate(anchor_.orientation);
            for(unsigned eye=0;eye<2;++eye){
                const auto& pose=frame.eye_poses[eye];
                eyes_[eye].position=rotate(inverse,{pose.position.x-anchor_.position.x,
                    pose.position.y-anchor_.position.y,pose.position.z-anchor_.position.z});
                eyes_[eye].orientation=multiply(inverse,normalized(pose.orientation));
            }
            sent_={};pending_feedback_={};sent_index_=pending_feedback_index_=0;have_feedback_=false;
        }
        const double seconds=double(frame.sampled_monotonic_ns-start_ns_)*1e-9;
        const double yaw=25*pi/180*std::sin(2*pi*seconds/4),pitch=12*pi/180*std::sin(2*pi*seconds/6);
        const double yaw_rate=25*pi/180*(2*pi/4)*std::cos(2*pi*seconds/4);
        const double pitch_rate=12*pi/180*(2*pi/6)*std::cos(2*pi*seconds/6);
        const UiQuat y{0,float(std::sin(yaw/2)),0,float(std::cos(yaw/2))};
        const UiQuat x{float(std::sin(pitch/2)),0,0,float(std::cos(pitch/2))};
        frame.head.pose=anchor_;frame.head.pose.orientation=normalized(multiply(anchor_.orientation,multiply(y,x)));
        frame.head.linear_velocity={};frame.head.linear_velocity_valid=true;
        frame.head.angular_velocity=rotate(anchor_.orientation,{float(pitch_rate*std::cos(yaw)),float(yaw_rate),float(-pitch_rate*std::sin(yaw))});
        frame.head.angular_velocity_valid=true;
        for(unsigned eye=0;eye<2;++eye){
            const auto offset=rotate(frame.head.pose.orientation,eyes_[eye].position);
            frame.eye_poses[eye]={{anchor_.position.x+offset.x,anchor_.position.y+offset.y,anchor_.position.z+offset.z},
                normalized(multiply(frame.head.pose.orientation,eyes_[eye].orientation))};
        }
        if(frame.sampled_monotonic_ns!=last_sample_ns_){++stats_.samples;last_sample_ns_=frame.sampled_monotonic_ns;}
        stats_.elapsed_seconds=seconds;stats_.yaw_degrees=yaw*180/pi;stats_.pitch_degrees=pitch*180/pi;
        return true;
    }
    void sent(const TrackingFrame& frame){
        if(!stats_.enabled)return;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!started_||frame.origin_epoch!=epoch_||!tracking_pose_valid(frame.head))return;
        ++stats_.sent_packets;
        const auto id=static_cast<std::int32_t>(frame.sequence&0x7fffffffu);
        // FrameData uploads the head quaternion, not the local eye rotation.
        // The returned eye may additionally include the PC's eye offset and
        // prediction, which remain legitimate diagnostic differences.
        const auto expected_head=normalized(frame.head.pose.orientation);
        sent_[sent_index_++%sent_.size()]={true,id,expected_head};
        // A received video response can race the post-send callback. Keep only
        // a bounded history and reconcile those responses exactly once here.
        for(auto& pending:pending_feedback_)if(pending.valid&&pending.id==id){
            matched(expected_head,pending.orientation);pending.valid=false;
        }
    }
    void feedback(std::int32_t id,std::uint64_t timestamp,UiQuat orientation){
        if(!stats_.enabled||!valid(orientation))return;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!started_||(have_feedback_&&timestamp==feedback_timestamp_))return;
        orientation=normalized(orientation);
        if(!have_feedback_){feedback_anchor_=orientation;have_feedback_=true;}
        feedback_timestamp_=timestamp;++stats_.feedback_frames;stats_.feedback_tracking_id=id;
        stats_.feedback_rotation_degrees=angle(feedback_anchor_,orientation);
        stats_.feedback_rotation_max_degrees=std::max(stats_.feedback_rotation_max_degrees,stats_.feedback_rotation_degrees);
        for(const auto& sent:sent_)if(sent.valid&&sent.id==id){
            matched(sent.orientation,orientation);return;
        }
        pending_feedback_[pending_feedback_index_++%pending_feedback_.size()]={true,id,orientation};
    }
    TestHeadMotionTelemetry snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return stats_;}
private:
    static constexpr double pi=3.14159265358979323846;
    static UiQuat multiply(UiQuat a,UiQuat b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
    static UiQuat conjugate(UiQuat q){return {-q.x,-q.y,-q.z,q.w};}
    static bool valid(UiQuat q){const double n=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;return std::isfinite(n)&&n>.5&&n<1.5;}
    static UiQuat normalized(UiQuat q){const auto n=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w);return {float(q.x/n),float(q.y/n),float(q.z/n),float(q.w/n)};}
    static UiVec3 rotate(UiQuat q,UiVec3 v){const auto result=multiply(multiply(q,{v.x,v.y,v.z,0}),conjugate(q));return {result.x,result.y,result.z};}
    static double angle(UiQuat a,UiQuat b){return 2*std::acos(std::clamp(std::abs(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w),0.,1.))*180/pi;}
    void matched(UiQuat expected_head,UiQuat returned_eye){++stats_.matched_feedback_frames;stats_.feedback_error_degrees=angle(expected_head,returned_eye);}
    struct PoseRecord {bool valid=false;std::int32_t id=0;UiQuat orientation;};
    mutable std::mutex mutex_;
    TestHeadMotionTelemetry stats_;
    bool started_=false,have_feedback_=false;
    std::uint64_t epoch_=0,start_ns_=0,last_sample_ns_=0,feedback_timestamp_=0,sent_index_=0,pending_feedback_index_=0;
    UiPose anchor_;std::array<UiPose,2> eyes_{};UiQuat feedback_anchor_;
    std::array<PoseRecord,1024> sent_{},pending_feedback_{};
};
} // namespace surfxr
