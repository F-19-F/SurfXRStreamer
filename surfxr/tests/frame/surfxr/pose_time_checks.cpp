#include "pose_clock.hpp"
#include "wire.hpp"
#include <iostream>
using namespace surfxr;
using namespace surfxr::transport;
namespace {
Bytes reply(std::uint64_t generation,std::uint64_t t1,std::uint64_t t2,std::uint64_t t3){
    Bytes b;pose_wire::put(b,pose_wire::clock_magic,4);pose_wire::put(b,1,4);
    for(auto n:{generation,t1,t2,t3})pose_wire::put(b,n,8);
    return b;
}
void queue_holdover_case(){
    PoseClock clock;constexpr std::uint64_t c0=10'000'000'000,s0=2'000'000'000;
    const auto truth=[&](std::uint64_t c){return s0+c-c0;};
    for(unsigned i=0;i<80;++i){
        const auto t1=c0+std::uint64_t(i)*250'000'000;
        const auto up=1'000'000ULL+(i==40?120'000'000:(i%8)*3'000'000ULL);
        const auto down=1'000'000ULL+(i%5)*2'000'000ULL,t4=t1+up+50'000+down;
        require(!clock.request(t1).empty(),"queue test request");
        const auto before=clock.stamp(t4+2'000'000'000,t4+2'000'000'000,1,i+1);
        require(clock.observe(reply(9,t1,truth(t1+up),truth(t1+up+50'000)),t4),"queued reply must remain a valid interval");
        const auto after=clock.stamp(t4+2'000'000'000,t4+2'000'000'000,1,i+1);
        require(after&&after->server_ns==truth(t4+2'000'000'000),"network queues changed the time mapping");
        if(before)require(before->server_ns==after->server_ns&&before->rate_ppb==after->rate_ppb,
            "asymmetric queue variation must not move the clock or its rate");
    }
    const auto late=c0+80'000'000'000;
    const auto before=clock.stamp(late+2'000'000,late+2'000'000,1,100);
    require(before&&before->server_ns==truth(late+2'000'000),"long reply loss changed the time mapping");
    clock.request(late);
    require(clock.observe(reply(9,late,truth(late+1'000'000),truth(late+1'000'000)),late+2'000'000),"reply recovery");
    const auto after=clock.stamp(late+2'000'000,late+2'000'000,1,101);
    require(after&&before->server_ns==after->server_ns,"reply recovery stepped the clock");
}
void startup_calibration_case(std::uint64_t c0,std::uint64_t s0,long double rate,bool reverse){
    // Real acquisition probes are 25 ms apart, not the 250 ms steady cadence
    // used by the long-running model below. A transition can queue one path.
    PoseClock clock;
    const auto truth=[&](std::uint64_t c){return s0+std::uint64_t(std::llround((c-c0)*rate));};
    std::deque<ClockExchange> replies;std::uint64_t first_ready=0,previous=0,poses=0;
    for(std::uint64_t dt=0;dt<=8'000'000'000;dt+=1'000'000){
        const auto now=c0+dt;
        if(!clock.request(now).empty()){
            std::uint64_t up=dt>=250'000'000&&dt<1'500'000'000?8'000'000:800'000,down=500'000;
            if(reverse)std::swap(up,down);
            replies.push_back({now,truth(now+up),truth(now+up+50'000),now+up+50'000+down});
        }
        while(!replies.empty()&&replies.front().t4<=now){
            const auto e=replies.front();replies.pop_front();
            const auto before=clock.stamp(e.t4,e.t4,1,dt+1);
            require(clock.observe(reply(9,e.t1,e.t2,e.t3),e.t4),"queued startup exchange rejected");
            const auto after=clock.stamp(e.t4,e.t4,1,dt+1);
            if(before&&after)require(before->server_ns==after->server_ns,"startup update stepped the clock phase");
        }
        const auto stamp=clock.stamp(now,now,1,dt+1);
        if(!stamp)continue;
        if(!first_ready){
            first_ready=now;
            const auto horizon=clock.status(now).t4+60'000'000'000;
            const auto coast=clock.stamp(horizon,horizon,1,dt+1);
            require(coast&&coast->error_ns>5'000'000,
                "uncertain holdover remains available with honest error telemetry");
        }
        require(stamp->server_ns>previous,"startup clock regressed");previous=stamp->server_ns;++poses;
        require(std::uint64_t(std::llabs(std::int64_t(stamp->server_ns)-std::int64_t(truth(now))))<=stamp->error_ns,
            "startup clock lost its ground-truth error bound");
    }
    require(first_ready&&first_ready-c0<=2'000'000&&poses>7900,"normal entry must start on the first measured reply, without a calibration wait");
}
void calibration_case(std::uint64_t c0,std::uint64_t s0,long double rate){
    PoseClock clock;const auto truth=[&](std::uint64_t c){return s0+std::uint64_t(std::llround((c-c0)*rate));};
    require(!clock.stamp(c0,c0,1,1),"no pose before measured calibration");
    std::uint64_t previous=0,last_reply=0,max_error=0;bool asymmetric_bias=false;
    for(unsigned i=0;i<100;++i){
        const auto t1=c0+std::uint64_t(i)*250'000'000;
        const auto up=2'000'000ULL+(i%7)*13'000+(i%13==12?30'000'000:0);
        const auto down=300'000ULL+(i%5)*53'000,processing=75'000ULL;
        const std::uint64_t t2=truth(t1+up),t3=truth(t1+up+processing),t4=t1+up+processing+down;
        const auto request=clock.request(t1);require(request.size()==16,"bounded calibration request");
        require(clock.request(t1+1).empty(),"calibration rate limit");
        const auto response=reply(9,t1,t2,t3);
        if(i==0){
            Writer control(true);control.integer(6,4);control.blob(response);
            require(server_control(control.bytes,true).config==response,"unchanged reliable clock envelope");
            for(std::size_t n=0;n<response.size();++n)
                require(!clock.observe(Bytes(response.begin(),response.begin()+n),t4),"truncated clock reply");
        }
        const auto before=clock.stamp(t4,t4,1,1000+i);
        require(clock.observe(response,t4),"asymmetric exchange with clock frequency difference");
        require(!clock.observe(response,t4),"clock replay rejected");last_reply=t4;
        const auto after=clock.stamp(t4,t4,1,1000+i);
        require(bool(after),"first measured exchange starts the timeline");
        if(!after)continue;
        if(before)require(before->server_ns==after->server_ns,"calibration update stepped the published time axis");
        const auto error=std::uint64_t(std::llabs(std::int64_t(after->server_ns)-std::int64_t(truth(t4))));
        require(error<=after->error_ns,"true asymmetric offset outside declared interval");
        require(after->error_ns>0&&after->calibration==i+1,"missing calibration evidence");
        asymmetric_bias|=error>100'000;max_error=std::max(max_error,error);
        const auto status=clock.status(t4);
        require(status.samples<=64&&status.ready,"bounded calibration history");
        require(rate*1e9>=status.rate_low_ppb-1&&rate*1e9<=status.rate_high_ppb+1,"measured drift interval excludes actual clock rate");
        for(unsigned tick=1;tick<=16;++tick){const auto c=t4+tick*2'000'000;
            const auto stamp=clock.stamp(c,c,1,2000+i*16+tick);require(bool(stamp),"calibrated pose disappeared");
            require(stamp->server_ns>previous,"mapped timestamps must remain monotonic");previous=stamp->server_ns;
            require(std::uint64_t(std::llabs(std::int64_t(stamp->server_ns)-std::int64_t(truth(c))))<=stamp->error_ns,"prediction of clock drift lost its error bound");
            const auto wire=pose_wire::encode(*stamp);
            require(wire.size()==80&&pose_wire::get(wire.data()+4,4)==2&&pose_wire::get(wire.data()+56,8)==stamp->error_ns,"v2 suffix contains calibration uncertainty");
        }
    }
    require(asymmetric_bias,"negative control must expose nonzero unknown one-way asymmetry");
    require(!clock.stamp(last_reply-201'000'000,last_reply,1,4000),"old pose cannot be restamped");
    const auto foreign=last_reply+250'000'000;clock.request(foreign);
    require(!clock.observe(reply(10,foreign,truth(foreign+1'000'000),truth(foreign+2'000'000)),foreign+3'000'000),"clock generation isolated");
    const auto future=last_reply+2'000'000'000;
    const auto continued=clock.stamp(future,future,1,5000);
    require(continued&&std::uint64_t(std::llabs(std::int64_t(continued->server_ns)-std::int64_t(truth(future))))<=continued->error_ns,
        "short reply gap must continue the same calibrated timeline without a phase reset");
    const auto t1=last_reply+2'250'000'000;clock.request(t1);
    require(!clock.observe(reply(9,t1,truth(t1+1'000'000)+50'000'000,truth(t1+2'000'000)+50'000'000),t1+3'000'000),"inconsistent clock step accepted");
    require(clock.status(t1+3'000'000).revision==100,"rejected observation changed calibration revision");
    const auto late=last_reply+60'000'000'000;
    const auto holdover=clock.stamp(late,late,1,5001);
    require(holdover&&std::uint64_t(std::llabs(std::int64_t(holdover->server_ns)-std::int64_t(truth(late))))<=holdover->error_ns,
        "reply loss must coast on the continuous clock with honest uncertainty");
    std::cout<<"calibration: independent epochs, rate="<<double((rate-1)*1e6)<<" ppm, max true error="<<max_error<<" ns; asymmetric bounds/continuous timeline passed\n";
}
}
void pose_time_checks(){
    queue_holdover_case();
    const auto extension=pose_wire::encode({1,2,3,4,5});
    Bytes expected{0x45,0x56,0x50,0x31,1,0,0,0};
    for(unsigned n=1;n<=5;++n){expected.push_back(n);expected.insert(expected.end(),7,0);}
    require(extension==expected&&extension.size()==48,"pose fixed LE stamp");
    calibration_case(10'000'000'000'000,2'000'000'000,1.000180L);
    calibration_case(2'000'000'000,10'000'000'000'000,.999780L);
    for(bool reverse:{false,true}){
        startup_calibration_case(10'000'000'000'000,2'000'000'000,1.000180L,reverse);
        startup_calibration_case(2'000'000'000,10'000'000'000'000,.999780L,reverse);
    }
    {
        PoseClock uncertain;constexpr std::uint64_t c=10'000'000'000,s=2'000'000'000;
        for(unsigned i=0;i<60;++i){const auto dt=std::uint64_t(i)*250'000'000;
            uncertain.request(c+dt);
            require(uncertain.observe(reply(7,c+dt,s+dt+10'000'000,s+dt+10'100'000),c+dt+20'100'000),"wide valid measurement");
            const auto stamp=uncertain.stamp(c+dt+21'000'000,c+dt+21'000'000,1,i+1);
            require(stamp&&stamp->error_ns>5'000'000,"wide measurements remain usable and report uncertainty without a hidden quality gate");
        }
        require(uncertain.status(c+15'000'000'001).ready,"wide calibration must not time out a healthy stream");
    }

    TrackingFrame frame;frame.head.flags=OrientationValid|PositionValid;
    frame.head.linear_velocity_valid=frame.head.angular_velocity_valid=true;
    frame.head.linear_velocity={1,2,3};frame.head.angular_velocity={4,5,6};
    for(auto& hand:frame.hands){hand.grip=frame.head;hand.grip_active=true;}
    for(bool timed:{false,true}){
        auto bytes=tracking(frame,123,true,true,timed);Reader input(bytes,true);
        require(input.duration()==123&&input.integer(8)==3,"same SURFXR tracking prefix and three devices");
        for(unsigned device=0;device<3;++device){input.integer(8);for(unsigned i=0;i<7;++i)input.real();
            for(unsigned i=1;i<=6;++i)require(input.real()==((device||timed)?float(i):0.f),"head and both grip velocities preserved only as negotiated");}
        for(unsigned i=0;i<7;++i)require(!input.boolean(),"unchanged SURFXR suffix");
        input.end();
    }
}
