#include "steamvr_boundary_ipc.hpp"
#include "boundary_sampling.hpp"
#include "boundary_settling.hpp"
#include "ipd_sampling.hpp"
#include "steamvr_settings.hpp"
#include "steamvr_controllers.hpp"
#include "headset_battery_status.hpp"
#include "surfxr/xr_boundary.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef SURFXR_HAVE_OPENVR_REFRESH
#include <openvr.h>
#endif

namespace surfxr {
int xr_boundary_worker(){
    // Existing collectors identify only the rendering process as surfxr-viewer.
    prctl(PR_SET_NAME,"surfxr-boundary",0,0,0);
    // Only the viewer's inherited private socket may invoke this entry. A shell
    // invocation must not initialize SteamVR or read the room boundary.
    int type=0;socklen_t size=sizeof(type);ucred peer{};socklen_t peer_size=sizeof(peer);
    if(getsockopt(STDIN_FILENO,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_SEQPACKET||
       getsockopt(STDIN_FILENO,SOL_SOCKET,SO_PEERCRED,&peer,&peer_size)||
       peer.uid!=getuid()||peer.pid!=getppid())return 1;
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    vr::EVRInitError error=vr::VRInitError_None;
    vr::VR_Init(&error,vr::VRApplication_Background);
    if(error!=vr::VRInitError_None)return 1;
    struct Shutdown{~Shutdown(){vr::VR_Shutdown();}} shutdown;
    std::uint64_t token=0;std::uint32_t samples=0;
    xr_detail::BoundarySnapshot packet{};bool pending=false;
    xr_detail::BoundarySettling settling;
    xr_detail::IpdSnapshot ipd;bool ipd_pending=false;
    xr_detail::IpdSampling ipd_sampling(10'000'000'000);
    xr_detail::SteamVrSettings refresh;
    xr_detail::RefreshSnapshot refresh_packet;bool refresh_pending=false;
    xr_detail::SteamVrControllers controllers;
    xr_detail::ControllerStatus controller_packet;bool controllers_pending=false;
    xr_detail::HeadsetBatteryStatus battery_packet;bool battery_pending=false;
    vr::HmdMatrix34_t last_origin{};bool have_origin=false;
    std::uint64_t last_universe=0;int last_calibration=0;
    const auto same_origin=[](const auto& a,const auto& b){
        std::array<float,12> first{},second{};
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c){first[r*4+c]=a.m[r][c];second[r*4+c]=b.m[r][c];}
        return !xr_detail::boundary_origin_changed(first,second);
    };
    for(;;){
        // Only drain runtime events on the timer; never reread geometry on it.
        pollfd input{STDIN_FILENO,POLLIN,0};
        const auto waited=poll(&input,1,100);
        if(waited<0){if(errno==EINTR)continue;return 1;}
        bool changed=false;
        if(input.revents&(POLLIN|POLLHUP|POLLERR)){
            for(unsigned i=0;i<4;++i){
                std::array<unsigned char,sizeof(xr_detail::RefreshRequest)> bytes{};
                const auto count=recv(STDIN_FILENO,bytes.data(),bytes.size(),MSG_TRUNC|MSG_DONTWAIT);
                if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
                if(count==sizeof(xr_detail::RefreshRequest)){
                    xr_detail::RefreshRequest request;std::memcpy(&request,bytes.data(),sizeof(request));
                    if(request.magic!=0x52465131||!std::isfinite(request.hz)||request.hz<0||request.hz>1000)return 1;
                    refresh.request(request);continue;
                }
                std::uint64_t next=0;std::memcpy(&next,bytes.data(),sizeof(next));
                if(count!=sizeof(next))return count==0?0:1;
                changed|=next!=token;token=next;pending=false;
            }
        }
        if(refresh.poll(refresh_packet))refresh_pending=true;
        if(refresh_pending){
            const auto sent=send(STDIN_FILENO,&refresh_packet,sizeof(refresh_packet),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(sent==sizeof(refresh_packet))refresh_pending=false;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;
        }
        auto* events=vr::VRSystem();vr::VREvent_t event{};bool ipd_changed=false,controllers_changed=false;
        bool inspect_metadata=false;
        if(events)for(unsigned i=0;i<64&&events->PollNextEvent(&event,sizeof(event));++i){
            switch(event.eventType){
            case vr::VREvent_TrackedDeviceActivated:
            case vr::VREvent_TrackedDeviceDeactivated:
            case vr::VREvent_TrackedDeviceUpdated:
            case vr::VREvent_TrackedDeviceRoleChanged:
            case vr::VREvent_WirelessDisconnect:
            case vr::VREvent_WirelessReconnect:controllers_changed=true;break;
            case vr::VREvent_PropertyChanged:
                controllers_changed|=event.trackedDeviceIndex==vr::k_unTrackedDeviceIndex_Hmd||
                    events->GetTrackedDeviceClass(event.trackedDeviceIndex)==vr::TrackedDeviceClass_Controller;break;
            case vr::VREvent_IpdChanged:
                if(event.trackedDeviceIndex==vr::k_unTrackedDeviceIndex_Hmd){ipd_changed=true;++ipd.events;}
                break;
            case vr::VREvent_ChaperoneDataHasChanged:
            case vr::VREvent_ChaperoneUniverseHasChanged:
            case 807: // RoomSetupFinished in Frame SDK; renamed RoomSetupCommitted upstream.
            case vr::VREvent_StandingZeroPoseReset:changed=true;break;
            case vr::VREvent_ChaperoneFlushCache:
                // Frame emits cache flushes even for unchanged geometry. Compare
                // cheap origin/calibration metadata before reading all walls.
                inspect_metadata=true;
                break;
            default:break;
            }
        }
        // Read once per batch, independently of the room-boundary switch.
        // The runtime's HMD-local property avoids world-space pose float noise.
        const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Cache-flush notifications may reflect tracking noise. Never turn
        // metadata polling into a periodic room-boundary update source.
        if(token&&events&&inspect_metadata){
            auto* chaperone=vr::VRChaperone();
            const auto origin=events->GetRawZeroPoseToStandingAbsoluteTrackingPose();
            const auto universe=events->GetUint64TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_CurrentUniverseId_Uint64);
            const int calibration=chaperone?int(chaperone->GetCalibrationState()):-1;
            if(!have_origin||!same_origin(last_origin,origin)||universe!=last_universe||calibration!=last_calibration){
                changed=true;last_origin=origin;last_universe=universe;last_calibration=calibration;have_origin=true;
            }
        }
        if(controllers.poll(controller_packet,now,controllers_changed)){
            controllers_pending=true;battery_packet=xr_detail::steamvr_headset_battery(now);battery_pending=true;
        }
        if(battery_pending){
            const auto sent=send(STDIN_FILENO,&battery_packet,sizeof(battery_packet),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(sent==sizeof(battery_packet))battery_pending=false;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;
        }
        if(controllers_pending){
            const auto sent=send(STDIN_FILENO,&controller_packet,sizeof(controller_packet),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(sent==sizeof(controller_packet))controllers_pending=false;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;
        }
        if(ipd_sampling.due(now,ipd_changed)){
            vr::ETrackedPropertyError property_error=vr::TrackedProp_Success;
            const auto value=events?events->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                vr::Prop_UserIpdMeters_Float,&property_error):0;
            ipd_sampling.sampled(property_error==vr::TrackedProp_Success?value:0,now);
            ipd.meters=ipd_sampling.value();++ipd.samples;ipd_pending=true;
        }
        if(ipd_pending){
            const auto sent=send(STDIN_FILENO,&ipd,sizeof(ipd),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(sent==sizeof(ipd))ipd_pending=false;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;
        }
        if(!token){settling.cancel();pending=false;}
        if(changed&&token){
            settling.request(now);
            // Withdraw the parent's cache before trying a replacement. Do not
            // let an unsent pre-event packet escape after a runtime change.
            packet={};packet.token=token;packet.sample_count=samples;pending=true;
        }
        if(token&&settling.due(now)){
        packet={};packet.token=token;packet.sample_count=++samples;
        std::array<float,12> sampled_origin{};std::uint64_t sampled_universe=0;
        auto* setup=vr::VRChaperoneSetup();auto* chaperone=vr::VRChaperone();auto* system=vr::VRSystem();
        if(setup&&chaperone&&system&&chaperone->GetCalibrationState()==vr::ChaperoneCalibrationState_OK){
            const auto before=system->GetRawZeroPoseToStandingAbsoluteTrackingPose();
            const auto universe_before=system->GetUint64TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_CurrentUniverseId_Uint64);
            std::array<vr::HmdQuad_t,StageBoundary::max_quads> quads{};std::uint32_t n=quads.size();
            // SteamVR may return false for a null-buffer count query. Read into
            // the bounded buffer directly and require a complete, valid result.
            if(setup->GetLiveCollisionBoundsInfo(quads.data(),&n)&&n>=3&&n<=quads.size()&&
               chaperone->GetPlayAreaSize(&packet.width,&packet.depth)){
                packet.count=n;
                for(unsigned i=0;i<n;++i)for(unsigned p=0;p<4;++p)for(unsigned axis=0;axis<3;++axis)
                    packet.quads[i][p*3+axis]=quads[i].vCorners[p].v[axis];
            }
            const auto after=system->GetRawZeroPoseToStandingAbsoluteTrackingPose();
            // A relocalization during the two reads must not publish mixed spaces.
            if(!same_origin(before,after))packet.count=0;
            sampled_universe=system->GetUint64TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_CurrentUniverseId_Uint64);
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)sampled_origin[r*4+c]=after.m[r][c];
            if(sampled_universe!=universe_before||chaperone->GetCalibrationState()!=vr::ChaperoneCalibrationState_OK)packet.count=0;
        }
        if(!settling.observe(packet,sampled_origin,sampled_universe,now))packet.count=0;
        pending=true;
        }
        if(token&&pending){
            const auto sent=send(STDIN_FILENO,&packet,sizeof(packet),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(sent==sizeof(packet))pending=false;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;
        }
    }
#else
    return 1;
#endif
}
}
