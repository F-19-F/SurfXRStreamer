#include "surfxr/vr_client_state.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <iterator>

namespace surfxr {
void VrClientState::set_surfxr_controls(std::function<bool(SettingChange)> setting,std::function<bool()> exit){
    std::lock_guard<std::mutex> lock(mutex_);surfxr_setting_=std::move(setting);surfxr_exit_=std::move(exit);
    stream_framerates_={60,72,80,90,120,144};max_quality_=7;
    auto caps=settings_.snapshot().capabilities;caps.surfxr_video_controls=true;settings_.set_capabilities(std::move(caps));capabilities_locked();
}
void VrClientState::set_mode_request(std::function<bool(bool)> request){
    std::lock_guard<std::mutex> lock(mutex_);mode_request_=std::move(request);
    auto caps=settings_.snapshot().capabilities;caps.session_mode_switch=bool(mode_request_);
    caps.can_launch_vr=bool(mode_request_);settings_.set_capabilities(std::move(caps));
}
namespace {
std::uint64_t now_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}

}
bool VrClientState::stage_ready(const TrackingFrame& f){return f.origin==TrackingOrigin::Stage&&f.stage_supported&&f.stage_locatable&&f.optics_valid&&tracking_pose_valid(f.head);}
VrClientState::VrClientState(SessionControlQueue& control,RemoteSettings& settings,const std::vector<float>& /*display_rates*/,bool microphone,
                           unsigned max_width,unsigned max_height,std::function<std::uint64_t()> clock)
    :control_(control),settings_(settings),microphone_available_(microphone),
     clock_(std::move(clock)),
     max_render_width_(std::min(max_width,8192u)),max_render_height_(std::min(max_height,8192u)){
    // The Frame runtime can enumerate only its currently selected 144 Hz mode.
    // Remote stream rate choices are independent of this display mode.
    stream_framerates_=protocol_vr_framerates();
    max_quality_=surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,max_render_width_,max_render_height_);
    if(max_quality_>=-1)settings_.seed_requested({SettingId::VrGraphicsQuality,double(std::min(2,max_quality_))});
    capabilities_locked();
}
void VrClientState::capabilities_locked(){
    auto c=settings_.snapshot().capabilities;
    const bool ready=have_tracking_&&stage_ready(tracking_);
    const auto stream_rate=connected_&&stats_.requested_vr&&source_known_&&received_source_==1?vr_frame_rate_:0u;
    auto status=stats_.requested_vr&&!stats_.presenting_vr?VrMenuStatus::Starting:VrMenuStatus::Idle;
    const auto error=[&](const char* name){return std::strcmp(stats_.error,name)==0;};
    if(error("stage_tracking_not_ready"))status=VrMenuStatus::StageNotReady;
    else if(error("no_supported_vr_refresh_rate"))status=VrMenuStatus::NoSupportedRefreshRate;
    else if(error("steamvr_not_available"))status=VrMenuStatus::SteamVrUnavailable;
    else if(error("control_queue_full"))status=VrMenuStatus::ControlQueueFull;
    else if(error("vr_start_timed_out"))status=VrMenuStatus::StartTimedOut;
    if(c.connected==connected_&&c.stage_active==ready&&c.vr_ready==ready&&c.vr_active==stats_.presenting_vr&&c.microphone_available==microphone_available_&&c.vr_framerates==stream_framerates_
       &&c.vr_requested==stats_.requested_vr&&c.vr_status==status&&c.max_vr_quality==max_quality_&&c.vr_stream_framerate==stream_rate)return;
    c.connected=connected_;c.stage_active=ready;c.vr_ready=ready;c.vr_active=stats_.presenting_vr;c.microphone_available=microphone_available_;c.vr_framerates=stream_framerates_;
    c.vr_requested=stats_.requested_vr;c.vr_status=status;
    c.vr_stream_framerate=stream_rate;
    c.max_vr_quality=max_quality_;c.server_video_controls=!vr_only_&&!desktop_only_;c.vr_only=vr_only_;
    settings_.set_capabilities(std::move(c));
}
void VrClientState::set_vr_only(bool value){
    std::lock_guard<std::mutex> lock(mutex_);
    if(connected_)throw std::logic_error("backend_change_during_connection");
    vr_only_=value;auto caps=settings_.snapshot().capabilities;caps.server_video_controls=!value;caps.vr_only=value;
    settings_.set_capabilities(std::move(caps));
}
void VrClientState::set_desktop_only(bool value){
    std::lock_guard<std::mutex> lock(mutex_);if(connected_)throw std::logic_error("backend_change_during_connection");desktop_only_=value;
    auto caps=settings_.snapshot().capabilities;caps.server_video_controls=!value&&!vr_only_;settings_.set_capabilities(std::move(caps));
}
void VrClientState::clear_video_locked(){
    for(auto& eye:eyes_)eye.clear();
    generations_={};stereo_.reset();presentation_.clear();
    presentation_candidate_.reset();have_presented_timestamp_=false;last_presented_timestamp_=0;
    presented_stereo_.reset();playout_anchored_=false;playout_source_ns_=playout_local_ns_=0;
    stats_.presenting_vr=false;
}
std::uint64_t VrClientState::clock_ns()const{return clock_?clock_():now_ns();}
void VrClientState::begin_vr_wait_locked(std::uint64_t now){
    if(vr_waiting_)return;
    if(frame_demand_enabled_&&!frame_demand_epoch_.load(std::memory_order_relaxed))now=frame_demand_paused_ns_;
    vr_wait_started_ns_=now;vr_waiting_=true;
    vr_config_wait_=source_known_&&received_source_==1;
    last_source_request_ns_=now;keyframe_recovery_attempted_=restart_recovery_attempted_=false;
}
void VrClientState::begin_desktop_wait_locked(std::uint64_t now){
    if(stats_.desktop_pending||desktop_failed_)return;
    if(!desktop_renderable_)now=desktop_paused_ns_;
    stats_.desktop_pending=true;desktop_wait_started_ns_=now;desktop_last_request_ns_=now;
    desktop_deadline_ns_=now+10'000'000'000ull;
    desktop_keyframe_attempted_=desktop_restart_attempted_=false;
}
void VrClientState::connected(bool value){
    std::lock_guard<std::mutex> lock(mutex_);connected_=value;
    if(value&&vr_only_){stats_.requested_vr=true;begin_vr_wait_locked(clock_ns());}
    if(!value){
        vr_frame_rate_=0;
        clear_video_locked();stats_.requested_vr=false;stats_.desktop_pending=false;stats_.start_command_pending=false;
        vr_waiting_=false;desktop_ready_=false;desktop_failed_=false;desktop_deadline_ns_=0;sent_.clear();
        source_known_=false;monitor_deadline_ns_=0;monitor_sent_=false;
        auto caps=settings_.snapshot().capabilities;caps.can_add_monitor=caps.can_remove_monitor=false;
        caps.frame_controller_inputs=false;
        caps.desktop_monitor_count=0;caps.monitor_pending=caps.monitor_timed_out=false;settings_.set_capabilities(std::move(caps));
    }
    capabilities_locked();
}
void VrClientState::control_command_sent(const SessionControlCommand& command){
    if(command.type!=SessionCommand::AddMonitor&&command.type!=SessionCommand::RemoveMonitor)return;
    std::lock_guard<std::mutex> lock(mutex_);
    if(connected_&&settings_.snapshot().capabilities.monitor_pending&&
       command.type==(monitor_add_?SessionCommand::AddMonitor:SessionCommand::RemoveMonitor)){
        monitor_sent_=true;monitor_deadline_ns_=clock_ns()+5'000'000'000ull;
    }
}
void VrClientState::tracking(const TrackingFrame& frame){
    std::lock_guard<std::mutex> lock(mutex_);
    if(have_tracking_&&frame.origin_epoch!=tracking_.origin_epoch){++stats_.origin_changes;clear_video_locked();}
    tracking_=frame;have_tracking_=true;capabilities_locked();tick_locked(clock_ns());
}
bool VrClientState::tracking_snapshot(TrackingFrame& frame)const{std::lock_guard<std::mutex> lock(mutex_);if(!have_tracking_)return false;frame=tracking_;return true;}
void VrClientState::tracking_sent(const TrackingFrame& frame){
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id=static_cast<std::int32_t>(frame.sequence&0x7fffffffu);
    if(!sent_.empty()&&sent_.back().id==id&&sent_.back().epoch==frame.origin_epoch)return;
    sent_.push_back({id,frame.origin_epoch,frame.sampled_monotonic_ns,frame.query_started_monotonic_ns,
                     frame.query_xr_time,frame.predicted_xr_time});
    while(sent_.size()>1024)sent_.pop_front();
    pair_locked();
}
bool VrClientState::enter_locked(){
    if(mode_request_)return mode_request_(false);
    return vr_only_&&connected_&&stats_.requested_vr;
}
bool VrClientState::desktop_locked(){
    if(mode_request_)return mode_request_(true);
    return desktop_only_&&connected_;
}
bool VrClientState::toggle(){std::lock_guard<std::mutex> lock(mutex_);return stats_.requested_vr?desktop_locked():enter_locked();}
bool VrClientState::launch(){std::lock_guard<std::mutex> lock(mutex_);return enter_locked();}
bool VrClientState::desktop(){std::lock_guard<std::mutex> lock(mutex_);return desktop_locked();}
bool VrClientState::monitor(bool add){
    std::lock_guard<std::mutex> lock(mutex_);
    auto caps=settings_.snapshot().capabilities;
    if(!connected_||stats_.requested_vr||caps.monitor_pending||
       !(add?caps.can_add_monitor:caps.can_remove_monitor)||
       (add&&caps.desktop_monitor_count>=4)||(!add&&caps.desktop_monitor_count==1))return false;
    if(!(add?control_.add_monitor():control_.remove_monitor())){stats_.error="control_queue_full";return false;}
    monitor_add_=add;monitor_previous_count_=caps.desktop_monitor_count;
    monitor_deadline_ns_=0;monitor_sent_=false;
    caps.monitor_pending=true;caps.monitor_timed_out=false;settings_.set_capabilities(std::move(caps));
    return true;
}
void VrClientState::desktop_monitors(unsigned count,unsigned capacity){
    if(!count||count>4)throw std::invalid_argument("invalid_desktop_monitor_count");
    if(capacity&&(capacity<count||capacity>4))throw std::invalid_argument("invalid_desktop_monitor_capacity");
    std::lock_guard<std::mutex> lock(mutex_);
    auto caps=settings_.snapshot().capabilities;caps.desktop_monitor_count=count;
    if(capacity){caps.can_add_monitor=count<capacity;caps.can_remove_monitor=count>1;
        caps.monitor_pending=caps.monitor_timed_out=false;monitor_deadline_ns_=0;}
    if((caps.monitor_pending||caps.monitor_timed_out)&&monitor_sent_&&(!monitor_previous_count_||
       (monitor_add_?count>monitor_previous_count_:count<monitor_previous_count_))){
        caps.monitor_pending=false;caps.monitor_timed_out=false;monitor_deadline_ns_=0;
    }
    settings_.set_capabilities(std::move(caps));
}
bool VrClientState::exit_vr(){
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ok=mode_request_&&surfxr_exit_&&surfxr_exit_()&&desktop_locked();
    if(ok)++stats_.exit_requests;
    return ok;
}
void VrClientState::recenter(){}

bool VrClientState::setting(SettingChange change){
    std::lock_guard<std::mutex> lock(mutex_);
    if(surfxr_setting_&&(static_cast<unsigned>(change.id)<=2||change.id==SettingId::DesktopPointerPriority)){const bool ok=surfxr_setting_(change);if(!ok)++stats_.settings_rejected;return ok;}
    if((change.id!=SettingId::MicrophoneEnabled&&change.id!=SettingId::MicrophoneVolume)||
       !setting_request_allowed(change,settings_.snapshot())){++stats_.settings_rejected;return false;}
    settings_.request(change);return settings_.observe(change);
}
void VrClientState::source_config(std::uint8_t source,std::uint32_t codec){
    std::lock_guard<std::mutex> lock(mutex_);
    if(source!=1)vr_frame_rate_=0;
    if(source!=(stats_.requested_vr?1u:0u))++stats_.source_mismatches;
    received_source_=source;source_known_=true;desktop_ready_=false;clear_video_locked();
    if(source==0){
        if(stats_.requested_vr)begin_vr_wait_locked(clock_ns());
        else begin_desktop_wait_locked(clock_ns());
    }else if(stats_.requested_vr){
        begin_vr_wait_locked(clock_ns());
        if(!vr_config_wait_){vr_config_wait_=true;vr_wait_started_ns_=frame_demand_enabled_&&!frame_demand_epoch_.load(std::memory_order_relaxed)?frame_demand_paused_ns_:clock_ns();}
    }else begin_desktop_wait_locked(clock_ns());
    auto c=settings_.snapshot().capabilities;c.active_codec=static_cast<int>(codec);if(source==1)c.has_vr=true;settings_.set_capabilities(std::move(c));capabilities_locked();
}
void VrClientState::desktop_decoded(unsigned stream){
    std::lock_guard<std::mutex> lock(mutex_);
    if(stream!=0||!connected_||!source_known_||received_source_!=0)return;
    desktop_ready_=true;
    if(stats_.requested_vr)return;
    // A configuration or a cached XR surface is not a new desktop image.
    // A late valid image may still win the race with the recovery deadline.
    stats_.desktop_pending=false;desktop_deadline_ns_=0;desktop_failed_=false;stats_.error="";
}
void VrClientState::vr_configuration(const VrConfiguration& config){
    if(config.stream_count!=(config.stereo_packed?1u:(config.foveated?4u:2u))||
       (config.stereo_packed&&config.foveated&&!config.gaze_atlas))throw std::runtime_error("unsupported_vr_stream_layout");
    if(config.codec!=1&&config.codec!=2&&config.codec!=5)throw std::runtime_error("vr_requires_h264_or_hevc8_hardware");
    // Publish source, observed dimensions and the derived quality limit under
    // one state lock. Otherwise an XR setting request can enqueue a quality
    // using the old cap after this configuration has already arrived.
    std::lock_guard<std::mutex> lock(mutex_);
    vr_codec_=static_cast<int>(config.codec);
    vr_frame_rate_=config.frame_rate;
    stats_.playout_reserve_ns=vr_only_&&config.frame_rate?
        std::min<std::uint64_t>(25'000'000,2'000'000'000ull/config.frame_rate):0;
    stream_count_=config.stereo_packed?(config.foveated?4:2):config.stream_count;
    reconstruction_width_=config.render_width;reconstruction_height_=config.render_height;
    settings_.observe({SettingId::FoveatedStreaming,config.foveated?1.0:0.0});
    if(!stats_.requested_vr)++stats_.source_mismatches;
    received_source_=1;source_known_=true;desktop_ready_=false;clear_video_locked();
    if(stats_.requested_vr){
        begin_vr_wait_locked(clock_ns());
        if(!vr_config_wait_){vr_config_wait_=true;vr_wait_started_ns_=frame_demand_enabled_&&!frame_demand_epoch_.load(std::memory_order_relaxed)?frame_demand_paused_ns_:clock_ns();}
    }
    else begin_desktop_wait_locked(clock_ns());
    auto capabilities=settings_.snapshot().capabilities;
    capabilities.active_codec=static_cast<int>(config.codec);capabilities.has_vr=true;
    capabilities.frame_controller_inputs=vr_only_&&config.frame_controller_inputs;
    settings_.set_capabilities(std::move(capabilities));
    if(!surfxr_setting_)settings_.observe({SettingId::VrFramerate,double(config.frame_rate)});
    if(!config.streams.empty()){
        const auto& stream=config.streams.front();
        settings_.observe_vr_dimensions(config.gaze_atlas?config.gaze_atlas.base_width:stream.width,
            config.gaze_atlas?config.gaze_atlas.base_height:stream.height,config.render_width,config.render_height,config.foveated);
    }
    capabilities_locked();
}
void VrClientState::set_renderable(bool renderable){
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_enabled=frame_demand_enabled_;
    const bool was_renderable=frame_demand_epoch_.load(std::memory_order_relaxed)!=0;
    if(was_enabled&&was_renderable==renderable)return;
    const auto now=clock_ns();
    frame_demand_enabled_=true;
    // Clear eyes as well as the completed/candidate pair. Clearing only the
    // presentation queue could pair a pre-pause eye with resumed output.
    clear_video_locked();
    if(renderable){
        if(was_enabled&&!was_renderable&&vr_waiting_&&now>=frame_demand_paused_ns_){
            const auto paused=now-frame_demand_paused_ns_;
            vr_wait_started_ns_+=paused;last_source_request_ns_+=paused;
        }
        // Never reuse zero (disabled) or an earlier published demand token.
        if(++next_frame_demand_epoch_==0)throw std::overflow_error("vr_frame_demand_epoch_exhausted");
        frame_demand_epoch_.store(next_frame_demand_epoch_,std::memory_order_release);
    }else{
        frame_demand_paused_ns_=now;
        frame_demand_epoch_.store(0,std::memory_order_release);
    }
    // Do not rearm recovery for an already running stream. Freeze the
    // one-shot recovery delays while invisible and shift them on resume.
    capabilities_locked();
}
void VrClientState::set_desktop_renderable(bool renderable){
    std::lock_guard<std::mutex> lock(mutex_);if(renderable==desktop_renderable_)return;
    const auto now=clock_ns();
    if(renderable&&stats_.desktop_pending&&now>=desktop_paused_ns_){
        const auto paused=now-desktop_paused_ns_;
        desktop_wait_started_ns_+=paused;desktop_last_request_ns_+=paused;desktop_deadline_ns_+=paused;
    }
    if(!renderable)desktop_paused_ns_=now;
    desktop_renderable_=renderable;
}
void VrClientState::decoded(unsigned eye,std::shared_ptr<const VideoFrame> frame){
    std::lock_guard<std::mutex> lock(mutex_);
    const auto demand=frame_demand_epoch_.load(std::memory_order_relaxed);
    if(frame_demand_enabled_&&(!demand||!frame||frame->output_epoch!=demand))return;
    if(eye>=stream_count_||!frame||!frame->vr_metadata||!frame->has_source_timestamp){++stats_.invalid_frames;++stats_.missing_metadata;return;}
    if(!frame->vr_metadata->pose_valid){++stats_.invalid_frames;++stats_.invalid_pose;return;}
    if(!frame->vr_metadata->fov_valid){++stats_.invalid_frames;++stats_.invalid_fov;return;}
    if(frame->source_timestamp!=frame->vr_metadata->timestamp){++stats_.invalid_frames;++stats_.mismatched_timestamp;return;}
    if(frame->generation==generations_[eye])return;
    generations_[eye]=frame->generation;eyes_[eye].push_back(std::move(frame));while(eyes_[eye].size()>3)eyes_[eye].pop_front();pair_locked();
}
void VrClientState::pair_locked(){
    if(frame_demand_enabled_&&!frame_demand_epoch_.load(std::memory_order_relaxed))return;
    if(!stats_.requested_vr||received_source_!=1||!have_tracking_)return;
    for(auto left=eyes_[0].rbegin();left!=eyes_[0].rend();++left){
        const auto stamp=(*left)->source_timestamp;
        if(stereo_&&stamp<=stereo_->metadata->timestamp)continue;
        const auto& meta=(*left)->vr_metadata;
        std::array<std::shared_ptr<const VideoFrame>,4> matched{};matched[0]=*left;
        bool complete=true;
        for(unsigned id=1;id<stream_count_;++id){
            const auto found=std::find_if(eyes_[id].rbegin(),eyes_[id].rend(),[&](const auto& f){return f->source_timestamp==stamp;});
            if(found==eyes_[id].rend()){complete=false;break;}
            if(!same_vr_presentation_metadata(*(*found)->vr_metadata,*meta)){++stats_.invalid_frames;complete=false;break;}
            matched[id]=*found;
        }
        if(!complete)continue;
        const auto epoch=std::find_if(sent_.rbegin(),sent_.rend(),[&](const auto& sent){return sent.id==meta->tracking_frame_id;});
        if(epoch==sent_.rend()){++stats_.unmatched_epochs;continue;}
        if(epoch->epoch!=tracking_.origin_epoch){++stats_.old_epochs;continue;}
        auto pair=std::make_shared<VrStereoFrame>();pair->foveated=stream_count_==4;
        if(pair->foveated){pair->reconstruction_width=reconstruction_width_;pair->reconstruction_height=reconstruction_height_;}
        pair->eyes={matched[0],matched[pair->foveated?2:1]};
        if(pair->foveated)pair->focus={matched[1],matched[3]};
        pair->metadata=meta;pair->origin_epoch=epoch->epoch;
        pair->pose_sampled_monotonic_ns=epoch->sampled_monotonic_ns;
        pair->pose_query_monotonic_ns=epoch->query_monotonic_ns;
        pair->pose_query_xr_time=epoch->query_xr_time;
        pair->pose_target_xr_time=epoch->target_xr_time;
        pair->ready_monotonic_ns=clock_ns();
        stereo_=std::move(pair);
        // SURFXR retains two intervals of playout slack in three bounded slots.
        // Completed eye history is already owned by the immutable pairs;
        // release redundant references before acquiring another CAPTURE lease.
        if(vr_only_){
            const auto local=stereo_->ready_monotonic_ns+stats_.playout_reserve_ns;
            if(!playout_anchored_){playout_anchored_=true;playout_source_ns_=stamp;playout_local_ns_=local;}
            else if(stamp>=playout_source_ns_&&local>=stamp-playout_source_ns_){
                // Startup IDR/decoder latency must not become permanent delay.
                // Track the fastest delivery floor; each image then waits at
                // most the reserve, even with a bounded three-frame queue.
                playout_local_ns_=std::min(playout_local_ns_,local-(stamp-playout_source_ns_));
            }
            for(auto& eye:eyes_)while(!eye.empty()&&eye.front()->source_timestamp<=stamp)eye.pop_front();
        }
        presentation_.push_back({stereo_,stereo_->ready_monotonic_ns});
        while(presentation_.size()>(vr_only_?3u:2u)){presentation_.pop_front();++stats_.paired_queue_overflows;}
        stats_.presenting_vr=true;++stats_.paired_frames;stats_.error="";vr_waiting_=false;capabilities_locked();return;
    }
}
std::shared_ptr<const VrStereoFrame> VrClientState::stereo()const{std::lock_guard<std::mutex> lock(mutex_);return stats_.requested_vr&&have_tracking_&&stage_ready(tracking_)?stereo_:nullptr;}
void VrClientState::tick_locked(std::uint64_t now){
    if(!connected_)return;
    if(monitor_deadline_ns_&&now>=monitor_deadline_ns_){
        auto caps=settings_.snapshot().capabilities;caps.monitor_pending=false;caps.monitor_timed_out=true;
        settings_.set_capabilities(std::move(caps));monitor_deadline_ns_=0;
    }
    if(!stats_.requested_vr){
        if(!stats_.desktop_pending||!desktop_renderable_)return;
        if(now>=desktop_deadline_ns_){
            ++stats_.desktop_timeouts;stats_.desktop_pending=false;desktop_failed_=true;desktop_deadline_ns_=0;
            stats_.error="desktop_switch_timed_out";return;
        }
        if(source_known_&&received_source_==0){
            if(!desktop_keyframe_attempted_&&now-desktop_wait_started_ns_>=3'000'000'000ull&&control_.force_keyframe()){
                desktop_keyframe_attempted_=true;++stats_.desktop_keyframe_recoveries;
            }
        }
        // The PC's same-value source setter does not restart a stalled capture.
        // Recover even when it never sent the desktop configuration. Admit the
        // desired source and restart together, preserving their wire order and
        // leaving the one-shot available when the bounded queue is full.
        if(!desktop_restart_attempted_&&now-desktop_wait_started_ns_>=6'000'000'000ull&&control_.restart_stream()){
            desktop_restart_attempted_=true;++stats_.desktop_restart_recoveries;
        }
        return;
    }
    if(frame_demand_enabled_&&!frame_demand_epoch_.load(std::memory_order_relaxed))return;
    if(stats_.start_command_pending||!vr_waiting_)return;
    // Runtime startup and waiting for the first stereo pair have no fixed
    // overall deadline. Only explicit user cancellation or a protocol/session
    // failure ends the request; an initialized PC may still be loading VR.
    // LaunchSteamVR creates the runtime first. Its native readiness callback
    // sets HasVR and normally selects VR automatically; retry the source only
    // after readiness, never during driver initialization.
    // A configuration is not a first video frame. Request one keyframe and
    // then one stream restart if the source switched but no valid pair arrived.
    // These bounded, one-shot recoveries never launch SteamVR or switch modes.
    if(received_source_==1&&vr_config_wait_&&!stats_.presenting_vr){
        if(!keyframe_recovery_attempted_&&now-vr_wait_started_ns_>=3'000'000'000ull){
            keyframe_recovery_attempted_=true;
            if(control_.force_keyframe())++stats_.vr_keyframe_recoveries;
        }
        if(!restart_recovery_attempted_&&now-vr_wait_started_ns_>=6'000'000'000ull){
            restart_recovery_attempted_=true;
            if(control_.restart_stream())++stats_.vr_restart_recoveries;
        }
    }
}
RemoteSettingsSnapshot VrClientState::settings_snapshot(){std::lock_guard<std::mutex> lock(mutex_);tick_locked(clock_ns());capabilities_locked();return settings_.snapshot();}
void VrClientState::tick(){std::lock_guard<std::mutex> lock(mutex_);tick_locked(clock_ns());}
VrClientStats VrClientState::statistics()const{std::lock_guard<std::mutex> lock(mutex_);auto result=stats_;result.paired_queue_depth=presentation_.size();result.desktop_failed=desktop_failed_;return result;}


} // namespace surfxr
