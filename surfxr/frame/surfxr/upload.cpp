#include "upload.hpp"
#include "unified.hpp"
#include "boundary_upload.hpp"
#include "view_updates.hpp"
#include "controller_motion.hpp"
#include "controller_state.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr::transport {
Upload::Upload(Connection& c,TrackingLedger& l,const SessionOptions& o,const SessionCallbacks& cb,float hz,bool gaze,bool frame_inputs,bool frame_skeletons,bool boundary_sync,bool controller_state,std::shared_ptr<PoseClock> pose_clock)
    :connection_(c),ledger_(l),options_(o),callbacks_(cb),hz_(hz),gaze_(gaze),frame_inputs_(frame_inputs),frame_skeletons_(frame_skeletons),boundary_sync_(boundary_sync),controller_state_(controller_state),pose_clock_(std::move(pose_clock)),worker_([this]{run();}){}
Upload::~Upload(){stopping_.store(true);if(worker_.joinable())worker_.join();}
void Upload::check(){std::lock_guard<std::mutex> lock(error_mutex_);if(error_)std::rethrow_exception(error_);}
void Upload::run(){
    try{
        auto deadline=Clock::now(),next_battery=deadline,next_report=deadline,next_skeleton=deadline,next_keyboard=deadline;bool keyboard_visible=false;
        const auto period=std::chrono::nanoseconds(static_cast<std::uint64_t>(1e9/hz_));
        Bytes last_buttons,last_space;std::uint64_t last_epoch=0,last_send=0;
        BoundaryUpload boundary;
        ControllerStateUpload controller_state;
        ViewUpdates view_updates;bool last_menu=false;
        ControllerMotionCache hand_cache;std::uint64_t last_sequence=0;
        TrackingUploadTelemetry telemetry;telemetry.requested_hz=static_cast<unsigned>(std::lround(hz_));
        const bool v21=is_v21(options_.surfxr_version);
        connection_.send_control(profile(0,v21,frame_inputs_));connection_.send_control(profile(1,v21,frame_inputs_));
        if(callbacks_.desktop_input_release)callbacks_.desktop_input_release();
        while(!stopping_.load()&&!(connection_.cancelled&&connection_.cancelled())){
            if(options_.surfxr_unified&&options_.surfxr_unified->keyboard()){
                const bool visible=options_.keyboard_overlay&&options_.keyboard_overlay->active.load();
                if(visible!=keyboard_visible||Clock::now()>=next_keyboard){
                    options_.surfxr_unified->keyboard_visibility(visible);keyboard_visible=visible;next_keyboard=Clock::now()+std::chrono::milliseconds(200);
                }
            }
            if(options_.surfxr_unified&&options_.surfxr_unified->keyboard()&&callbacks_.desktop_input)
                for(unsigned i=0;i<64;++i){desktop::Event event;if(!callbacks_.desktop_input(event))break;if(event.kind==desktop::EventKind::Key||event.kind==desktop::EventKind::Unicode||event.kind==desktop::EventKind::Release||(event.candidate&&options_.keyboard_overlay&&options_.keyboard_overlay->clickable.load()))connection_.send_control(desktop::input(event));}
            if(pose_clock_){const auto request=pose_clock_->request(now_ns());if(!request.empty()){
                Writer w(true);w.integer(11,4);w.blob(request);connection_.send_control(w.bytes);
            }}
            TrackingFrame frame;
            const bool sampled=pose_clock_?(callbacks_.sample_tracking&&callbacks_.sample_tracking(frame)):
                (callbacks_.tracking_frame&&callbacks_.tracking_frame(frame));
            // A callback may publish a newly sampled pose while we read it.
            // Compare its clock against a time acquired after the snapshot.
            const auto now=now_ns();
            const auto pose_stamp=pose_clock_?pose_clock_->stamp(frame.sampled_monotonic_ns,now,frame.origin_epoch,frame.sequence):std::optional<pose_wire::Stamp>{};
            const bool fresh=sampled&&(!pose_clock_||pose_stamp)&&frame.origin==TrackingOrigin::Stage&&
                frame.stage_supported&&frame.stage_locatable&&frame.optics_valid&&tracking_pose_valid(frame.head)&&
                frame.sampled_monotonic_ns&&frame.sampled_monotonic_ns<=now&&now-frame.sampled_monotonic_ns<=250'000'000;
            // SYNCHRONIZED sessions can provide valid head poses while the
            // runtime pauses presentation. Keep SURFXR's pose/timestamp pipeline
            // alive; visibility/focus still gate game buttons and hand motion.
            const bool controllers=!callbacks_.vr_controllers_enabled||callbacks_.vr_controllers_enabled();
            if(v21&&controller_state_)if(const auto* bytes=controller_state.update(controllers,now))connection_.send_control(*bytes);
            if(frame_skeletons_&&Clock::now()>=next_skeleton){
                for(unsigned hand=0;hand<2;++hand)connection_.send_control(frame_skeleton(frame,hand,fresh&&controllers,now));
                next_skeleton=Clock::now()+std::chrono::milliseconds(10);
            }
            // Release game buttons on stale tracking, focus loss, UI capture or
            // disabled forwarding. No stale controller motion is extrapolated.
            if(!fresh){frame.focused=false;frame.visible=false;}
            telemetry.focused=frame.focused;
            for(unsigned i=0;i<2;++i){const auto& hand=frame.hands[i];
                telemetry.hand_pose_active[i]=fresh&&controllers&&frame.focused&&tracking_controller_connected(hand)&&hand.grip_active&&tracking_pose_valid(hand.grip);
                telemetry.hand_input_active[i]=fresh&&controllers&&frame.focused&&tracking_controller_connected(hand)&&(hand.buttons_active||hand.analog_active);
                telemetry.hand_ui_captured[i]=frame.ui_captured[i];
            }
            const auto button_packet=buttons(frame,controllers&&fresh,v21,frame_inputs_);
            if(button_packet!=last_buttons){connection_.send_control(button_packet);last_buttons=button_packet;}
            const bool menu=controllers&&fresh&&frame.focused&&frame.visible&&
                (!frame.ui_captured[0]||frame.hands[0].profile==TrackingProfile::SteamFrame)&&
                (touch_controller_mapping(frame)[0].buttons&ButtonMenu);
            if(menu&&!last_menu)++telemetry.menu_press_edges;
            last_menu=menu;
            // Battery samples have their own freshness clocks. A paused or
            // unlocatable pose must not freeze the PC's battery properties.
            if(Clock::now()>=next_battery){
                const auto send_battery=[&](const char* path,const DeviceBattery& b){if(b.percent>=0&&b.percent<=100)connection_.send_control(battery(path_id(path),b.percent/100.f,b.charging_known&&b.charging,v21));};
                const auto battery_value=fresh_battery_snapshot(frame.battery,now);
                send_battery("/user/head",battery_value.hmd);send_battery("/user/hand/left",battery_value.controllers[0]);send_battery("/user/hand/right",battery_value.controllers[1]);next_battery=Clock::now()+std::chrono::seconds(5);
            }
            if(fresh){
                if(view_updates.should_send(frame,now)){
                    connection_.send_control(views(frame,v21));view_updates.sent(frame,now);++telemetry.view_config_packets;
                }
                const auto space=playspace(frame,v21);
                if(space!=last_space||frame.origin_epoch!=last_epoch){connection_.send_control(space);last_space=space;}
                if(boundary_sync_){if(const auto* bytes=boundary.update(frame,now))connection_.send_control(*bytes);}
                last_epoch=frame.origin_epoch;
                auto wire_frame=frame;
                if(!pose_clock_)hand_cache.apply(wire_frame,telemetry.hand_pose_active,controllers);
                else for(unsigned i=0;i<2;++i)if(!telemetry.hand_pose_active[i])wire_frame.hands[i].grip_active=false;
                auto body=tracking(wire_frame,now,controllers,v21,bool(pose_clock_));
                if(gaze_){GazeSample sample;const bool available=frame.focused&&frame.visible&&callbacks_.gaze_sample&&callbacks_.gaze_sample(sample);
                    const auto feedback=gaze_feedback(available?&sample:nullptr,now,frame.origin_epoch);
                    body.insert(body.end(),feedback.begin(),feedback.end());}
                if(pose_stamp){const auto extension=pose_wire::encode(*pose_stamp);body.insert(body.end(),extension.begin(),extension.end());}
                // Register before write to cover a fast loopback server response.
                // A failed write terminates this session and cannot present it.
                ledger_.sent(now,frame);const auto before=now_ns();connection_.send_stream(0,body);const auto after=now_ns();
                if(callbacks_.tracking_sent)callbacks_.tracking_sent(frame);
                ++telemetry.packets;
                for(unsigned i=0;i<2;++i){
                    if(telemetry.hand_pose_active[i])++telemetry.fresh_hand_packets[i];else ++telemetry.cached_hand_packets[i];
                }
                if(last_sequence==frame.sequence)++telemetry.repeated_pose_packets;else ++telemetry.fresh_pose_packets;
                last_sequence=frame.sequence;telemetry.encoded_bytes_sum+=body.size();
                telemetry.encoded_bytes_max=std::max(telemetry.encoded_bytes_max,std::uint64_t{body.size()});
                telemetry.send_duration_ns_sum+=after-before;telemetry.send_duration_ns_max=std::max(telemetry.send_duration_ns_max,after-before);
                ++telemetry.sample_age_samples;telemetry.sample_age_ns_sum+=now-frame.sampled_monotonic_ns;
                telemetry.sample_age_ns_max=std::max(telemetry.sample_age_ns_max,now-frame.sampled_monotonic_ns);
                if(last_send){const auto elapsed=before-last_send;++telemetry.interval_samples;telemetry.interval_ns_sum+=elapsed;telemetry.interval_ns_max=std::max(telemetry.interval_ns_max,elapsed);}last_send=before;
            }else {
                ++telemetry.invalid_pose_packets;
            }
            if(options_.surfxr_feedback){Feedback value;while(options_.surfxr_feedback->pop(value)){
                Writer w(v21);for(auto ns:{value.timestamp,value.interval,value.decode,std::uint64_t{0},value.rendering,std::uint64_t{0},value.total})w.duration(ns);
                connection_.send_stream(4,w.bytes);
            }}
            if(Clock::now()>=next_report){telemetry.sample_monotonic_ns=now_ns();
                if(pose_clock_){const auto s=pose_clock_->status(telemetry.sample_monotonic_ns);
                    telemetry.clock_ready=s.ready;telemetry.clock_samples=s.samples;telemetry.clock_accepted=s.accepted;
                    telemetry.clock_rejected=s.rejected;telemetry.clock_revision=s.revision;telemetry.clock_error_ns=s.error_ns;
                    telemetry.clock_reply_age_ns=s.reply_age_ns;telemetry.clock_min_rtt_ns=s.min_rtt_ns;telemetry.clock_offset_ns=s.offset_ns;
                    telemetry.clock_rate_ppb=s.rate_ppb;telemetry.clock_rate_low_ppb=s.rate_low_ppb;telemetry.clock_rate_high_ppb=s.rate_high_ppb;
                    telemetry.clock_t1=s.t1;telemetry.clock_t2=s.t2;telemetry.clock_t3=s.t3;telemetry.clock_t4=s.t4;}
                if(callbacks_.tracking_telemetry)callbacks_.tracking_telemetry(telemetry);
                next_report=Clock::now()+std::chrono::seconds(1);}
            deadline+=period;if(Clock::now()>deadline){++telemetry.missed_deadlines;deadline=Clock::now()+period;}
            std::this_thread::sleep_until(deadline);
        }
        if(callbacks_.tracking_telemetry){telemetry.sample_monotonic_ns=now_ns();callbacks_.tracking_telemetry(telemetry);}
    }catch(const Cancelled&){}
    catch(...){std::lock_guard<std::mutex> lock(error_mutex_);error_=std::current_exception();}
}
}
