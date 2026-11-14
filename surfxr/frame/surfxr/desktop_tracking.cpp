#include "desktop_tracking.hpp"
#include "unified.hpp"
#include "boundary_upload.hpp"
#include "view_updates.hpp"
#include "surfxr/surfxr_unified.hpp"
#include <algorithm>
#include <poll.h>
#include <unistd.h>

namespace surfxr::transport {
DesktopTracking::DesktopTracking(const SessionOptions& o,const SessionCallbacks& cb,const std::atomic_bool* stop)
    :options_(o),callbacks_(cb),stop_(stop),worker_([this]{run();}){}
DesktopTracking::~DesktopTracking(){stopping_.store(true);worker_.join();}
bool DesktopTracking::cancelled()const{return stopping_.load()||(stop_&&stop_->load());}
void DesktopTracking::run(){
    std::array<char,64> hostname{};if(::gethostname(hostname.data(),hostname.size()-1))return;
    for(auto& c:hostname)if(c&&!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))c='-';
    const float hz=options_.surfxr_refresh_rates.empty()?float(options_.initial_vr_framerate):
        *std::max_element(options_.surfxr_refresh_rates.begin(),options_.surfxr_refresh_rates.end());
    const auto period=std::chrono::nanoseconds(static_cast<std::uint64_t>(1e9/hz));
    TrackingUploadTelemetry telemetry;telemetry.requested_hz=static_cast<unsigned>(hz);
    while(!cancelled()){
        try{
            Connection c;c.modern=true;c.cancelled=[this]{return cancelled();};
            const auto caps=capabilities(options_.host,options_.surfxr_view_width,options_.surfxr_view_height,{hz},
                options_.surfxr_version,false,true,false,false,hostname.data(),false,true);
            if(options_.surfxr_unified)c.control_socket=options_.surfxr_unified->tracking(caps);
            else {c.control_socket=Socket::connect_tcp(options_.host,options_.surfxr_control_port,options_.local_address,
                Clock::now()+std::chrono::milliseconds(500),c.cancelled);c.send_control(caps);}
            Framer framer(4096,true);
            auto reply=server_control(receive_control(c.control_socket,framer,Clock::now()+std::chrono::seconds(1),c.cancelled),true);
            if(reply.type==5&&reply.reserved=="frame_session_stopped")return;
            require(reply.type==5&&(reply.reserved=="frame_desktop_tracking_on"||reply.reserved=="frame_desktop_tracking_off"),"surfxr_desktop_tracking_not_supported");
            bool enabled=reply.reserved=="frame_desktop_tracking_on";
            bool boundary_sync=false,optics_sync=false;BoundaryUpload boundary;ViewUpdates view_updates;
            auto next=Clock::now(),keepalive=next,report=next,last_received=next;
            Bytes last_space;std::uint64_t epoch=0;
            while(!cancelled()){
                const auto tick=Clock::now();
                if(tick-last_received>(options_.surfxr_unified?unified::stall_timeout:std::chrono::seconds(5)))throw std::runtime_error("surfxr_desktop_tracking_timeout");
                if(c.control_socket.readable()){
                    reply=server_control(receive_control(c.control_socket,framer,tick+std::chrono::milliseconds(500),c.cancelled),true);
                    last_received=Clock::now();
                    if(reply.type==5&&reply.reserved=="frame_session_stopped")return;
                    if(reply.type==2)break;
                    if(reply.type==5&&reply.reserved=="frame_boundary_sync_v1")boundary_sync=true;
                    else if(reply.type==5&&reply.reserved=="frame_desktop_optics_v1")optics_sync=true;
                    else if(reply.type==5&&(reply.reserved=="frame_desktop_tracking_on"||reply.reserved=="frame_desktop_tracking_off")){
                        const bool next_enabled=reply.reserved=="frame_desktop_tracking_on";
                        if(next_enabled&&!enabled){last_space.clear();boundary=BoundaryUpload{};view_updates={};}enabled=next_enabled;}
                }
                if(tick>=keepalive){c.send_control(control(2,true));keepalive=tick+std::chrono::seconds(1);}
                if(enabled){
                    TrackingFrame frame;const bool sampled=callbacks_.tracking_frame&&callbacks_.tracking_frame(frame);
                    const auto now=now_ns();
                    const bool fresh=sampled&&frame.origin==TrackingOrigin::Stage&&frame.stage_locatable&&
                        tracking_pose_valid(frame.head)&&frame.sampled_monotonic_ns&&frame.sampled_monotonic_ns<=now&&now-frame.sampled_monotonic_ns<=250'000'000;
                    if(fresh){
                        // Old servers reject LocalViewParams on this connection.
                        // Share VR's accumulated-change filter after negotiation.
                        if(optics_sync&&view_updates.should_send(frame,now)){
                            c.send_control(views(frame,true));view_updates.sent(frame,now);++telemetry.view_config_packets;
                        }
                        if(boundary_sync){if(const auto* bytes=boundary.update(frame,now))c.send_control(*bytes);}
                        else{const auto space=playspace(frame,true);
                            if(space!=last_space||frame.origin_epoch!=epoch){c.send_control(space);last_space=space;epoch=frame.origin_epoch;}}
                        // Reuse the VR TrackingData bytes. No desktop pointer or
                        // game button events enter this pose-only connection.
                        Writer w(true);w.integer(11,4);w.blob(tracking(frame,now,!callbacks_.vr_controllers_enabled||callbacks_.vr_controllers_enabled(),true));
                        c.send_control(w.bytes);++telemetry.packets;
                        if(callbacks_.tracking_sent)callbacks_.tracking_sent(frame);
                    }else {
                        ++telemetry.invalid_pose_packets;
                    }
                }
                if(tick>=report){telemetry.sample_monotonic_ns=now_ns();if(callbacks_.tracking_telemetry)callbacks_.tracking_telemetry(telemetry);report=tick+std::chrono::seconds(1);}
                next+=period;if(next<Clock::now())next=Clock::now()+period;
                std::this_thread::sleep_until(next);
            }
        }catch(const std::exception&){}catch(const Cancelled&){}
        const auto retry=Clock::now()+std::chrono::seconds(2);
        while(!cancelled()&&Clock::now()<retry)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
