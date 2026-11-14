#include "surfxr/haptics.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
void pulse(const surfxr::HapticCommand& c,float amplitude,const char* reason){
    require(c.changed&&c.amplitude==amplitude&&c.duration_ns==30'000'000,reason);
}
void stop(const surfxr::HapticCommand& c,const char* reason){require(c.changed&&c.amplitude==0&&c.duration_ns==0,reason);}
int tests(){
    int groups=0;
    {
        surfxr::HapticsQueue queue;unsigned left=0,right=0;
        for(std::uint64_t frame=0;frame<120;++frame){
            const auto now=1'000'000'000+frame*8'333'333;
            const float amplitude=frame%2?.75f:.25f;
            require(queue.ingest_video_amplitudes(frame+1,1,{1.f,amplitude},now-2'000'000),"surfxr_dense_level_accepted");
            const auto commands=queue.consume(now,true);
            if(commands[0].changed){++left;require(commands[0].amplitude==1.f&&commands[0].duration_ns<=30'000'000,"surfxr_renewal_bounded");}
            if(commands[1].changed){++right;require(commands[1].amplitude==amplitude,"changed_strength_applied_immediately");}
        }
        require(left>=30&&left<=65&&right==120&&queue.statistics().deferred_renewals>0,"steady_level_not_restarted_every_video_frame");
        const auto commands=queue.consume(2'030'000'000,true);
        stop(commands[0],"surfxr_stall_stops_left");stop(commands[1],"surfxr_stall_stops_right");
    }++groups;
    {
        surfxr::HapticsQueue queue;
        queue.ingest_video_amplitudes(1,1,{1.f,0},1'000'000'000);
        pulse(queue.consume(1'000'000'000,true)[0],1.f,"surfxr_initial_short_pulse_immediate");
        queue.ingest_video_amplitudes(2,1,{1.f,0},1'008'000'000);
        require(!queue.consume(1'008'000'000,true)[0].changed,"surfxr_repeated_level_preserves_running_effect");
        queue.ingest_video_amplitudes(3,1,{0,0},1'009'000'000);
        const auto tail=queue.consume(1'025'000'000,true)[0];
        require(tail.changed&&tail.amplitude==1.f&&tail.duration_ns==13'000'000,"surfxr_deferred_tail_ends_30ms_after_last_positive");
        require(!queue.consume(1'037'999'999,true)[0].changed,"surfxr_tail_remains_active");
        stop(queue.consume(1'038'000'000,true)[0],"surfxr_zero_does_not_add_another_full_pulse");
        queue.ingest_video_amplitudes(4,1,{1.f,0},1'050'000'000);
        pulse(queue.consume(1'050'000'000,true)[0],1.f,"surfxr_next_distinct_pulse_immediate");
        queue.ingest_video_amplitudes(5,1,{.5f,0},1'051'000'000);
        pulse(queue.consume(1'051'000'000,true)[0],.5f,"surfxr_strength_change_not_rate_limited");
        queue.consume(1'052'000'000,false);
        queue.ingest_video_amplitudes(6,1,{1.f,0},1'053'000'000);
        require(!queue.consume(1'054'000'000,true)[0].changed,"surfxr_background_level_never_replayed");
    }++groups;
    return groups;
}
}
int main(int argc,char** argv){
    try{
        if(argc!=2||std::strcmp(argv[1],"--self-test")!=0)throw std::runtime_error("expected_self_test");
        const auto groups=tests();std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"haptics_self_test\":true,\"groups\":"<<groups<<"}\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
