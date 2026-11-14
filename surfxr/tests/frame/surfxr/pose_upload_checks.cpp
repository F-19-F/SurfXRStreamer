#include "upload.hpp"
#include <fcntl.h>
#include <sys/socket.h>
namespace surfxr::transport {
void pose_upload_checks(){
    int control_pair[2],data_pair[2];
    require(!socketpair(AF_UNIX,SOCK_STREAM,0,control_pair)&&!socketpair(AF_UNIX,SOCK_DGRAM,0,data_pair),"pose isolated socket pairs");
    for(int fd:{control_pair[0],control_pair[1],data_pair[0],data_pair[1]})fcntl(fd,F_SETFL,O_NONBLOCK);
    Connection connection;connection.modern=true;connection.tcp=false;connection.packet_size=1400;
    connection.control_socket=Socket(control_pair[0]);connection.stream_socket=Socket(data_pair[0]);
    Socket control(control_pair[1]),data(data_pair[1]);Framer framer(65536,true);Reassembler reassembler(1400,true);
    auto clock=std::make_shared<PoseClock>();TrackingLedger ledger;SessionOptions options;options.surfxr_version="21.0.0-dev14";
    SessionCallbacks callbacks;std::uint64_t queries=0;
    callbacks.tracking_frame=[](TrackingFrame&){fail("timed upload cannot reuse legacy cache");return false;};
    callbacks.sample_tracking=[&](TrackingFrame& f){
        f.origin=TrackingOrigin::Stage;f.origin_epoch=1;f.sequence=++queries;f.sampled_monotonic_ns=now_ns();
        f.stage_supported=f.stage_locatable=f.optics_valid=f.focused=f.visible=true;f.ipd_meters=.064f;
        f.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f}}};
        f.head.flags=OrientationValid|PositionValid;f.head.pose.position.x=float(queries);
        f.head.linear_velocity={1,2,3};f.head.angular_velocity={4,5,6};f.head.linear_velocity_valid=f.head.angular_velocity_valid=true;
        for(auto& hand:f.hands){hand.grip=f.head;hand.grip_active=true;}
        return true;
    };
    unsigned packets=0;std::uint64_t last_sequence=0;bool synchronized=false;
    const auto client_origin=now_ns();
    const auto server_time=[&](std::uint64_t client){return std::uint64_t(2'000'000'000ULL+std::llround((client-client_origin)*1.000180L));};
    {
        Upload upload(connection,ledger,options,callbacks,480,false,false,false,false,false,clock);
        const auto started=Clock::now(),deadline=started+std::chrono::seconds(5);
        while((packets<16||Clock::now()-started<std::chrono::seconds(4))&&Clock::now()<deadline){
            std::uint8_t bytes[65536];auto n=control.read(bytes,sizeof(bytes),true);
            if(n)framer.feed(bytes,n,[&](Bytes b){Reader r(b,true);if(r.integer(4)!=11)return;
                const auto request=r.blob();r.end();require(request.size()==16,"upload clock request");
                if(synchronized)return; // Calibration replies disappear; media/tracking stay healthy.
                const auto t1=pose_wire::get(request.data()+8,8),t2=server_time(now_ns());
                Bytes response;pose_wire::put(response,pose_wire::clock_magic,4);pose_wire::put(response,1,4);
                for(auto value:{std::uint64_t{42},t1,t2,t2})pose_wire::put(response,value,8);
                require(clock->observe(response,now_ns()),"upload clock accepted");
                synchronized=clock->status(now_ns()).ready;
            });
            n=data.read(bytes,sizeof(bytes),false);
            if(n)if(auto packet=reassembler.accept(bytes,n,now_ns())){
                require(synchronized&&packet->stream==0,"no pose before clock mapping");
                const auto& b=packet->bytes;require(b.size()>80,"calibrated tracking extension present");
                const auto* stamp=b.data()+b.size()-80;
                require(pose_wire::get(stamp,4)==pose_wire::stamp_magic&&pose_wire::get(stamp+8,8)==42,"session stamp at suffix");
                require(pose_wire::get(stamp+4,4)==2&&pose_wire::get(stamp+48,8)==1,"first calibration retained during reply loss");
                const auto seq=pose_wire::get(stamp+24,8),sample=pose_wire::get(stamp+40,8);
                require(seq>last_sequence,"every packet has a new sample");last_sequence=seq;
                const auto mapped=pose_wire::get(stamp+32,8),error=pose_wire::get(stamp+56,8);
                require(error>0,"honest calibration uncertainty included");
                require(std::llabs(std::int64_t(mapped)-std::int64_t(server_time(sample)))<=std::int64_t(error),"independent epochs and drift map within declared bound");
                Reader r(b.data(),b.size()-80,true);const auto poll=r.duration();
                require(pose_wire::get(stamp+72,8)<=poll,"calibration retains local measurement time");
                require(poll>=sample&&poll-sample<200'000'000&&r.integer(8)==3,"three devices with preserved sample/send times");
                for(unsigned device=0;device<3;++device){r.integer(8);for(unsigned i=0;i<4;++i)r.real();
                    require(r.real()==float(seq),"fresh pose serialized for head and both controllers");r.real();r.real();
                    for(unsigned v=1;v<=6;++v)require(r.real()==float(v),"upload preserves velocities");}
                for(unsigned i=0;i<7;++i)r.boolean();
                r.end();++packets;
            }
            upload.check();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    require(packets>=16,"timed upload sustains fresh samples through reply loss");
}
}
