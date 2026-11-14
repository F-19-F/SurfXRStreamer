#include "surfxr/motion_sync.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
void near(double actual,double expected,const char* reason){require(std::abs(actual-expected)<1e-12,reason);}
int tests(){
    int groups=0;
    {surfxr::MotionSync sync;
        const auto initial=sync.target(3'000'000'000);
        require(initial.valid&&initial.xr_time==3'000'000'000&&initial.lead_seconds==0,"initial_target_uses_query_not_display_time");
        require(!sync.target(0).valid&&!sync.target(-1).valid,"invalid_xr_query_time");
        require(sync.target(std::numeric_limits<std::int64_t>::max()).valid,"zero_lead_at_max_xr_time");
        require(sync.statistics().updates==0,"target_queries_do_not_train_estimate");
    }++groups;
    {surfxr::MotionSync surfxr;surfxr.set_epoch(1);
        // The observed pipeline submits near 38ms but displays near 60ms.
        // Predicting only through submission leaves a moving head ~22ms old.
        for(std::uint64_t id=1;id<=1000;++id){
            const auto query=id*1'000'000'000;
            require(surfxr.observe_display_frame(1,id,query,query+38'000'000,query+60'000'000),"surfxr_display_sample");
        }
        require(surfxr.target(1'000'000'000).xr_time==1'060'000'000,"surfxr_includes_remaining_compositor_wait");
        near(surfxr.statistics().previous_draw_seconds,0,"surfxr_does_not_add_gpu_or_previous_draw_twice");
        const auto saved=surfxr.statistics().latency_seconds;
        require(!surfxr.observe_display_frame(1,1000,1,80'000'001,90'000'001),"surfxr_reused_picture_not_training");
        require(!surfxr.observe_display_frame(1,999,1,80'000'001,90'000'001),"surfxr_older_picture_not_training");
        require(!surfxr.observe_display_frame(2,1001,1,38'000'001,60'000'001),"surfxr_epoch_mismatch");
        for(const auto times: {std::array<std::uint64_t,3>{0,38,60},{20,19,60},{20,40,0},{20,61,60},{1,38,1'000'000'001}})
            require(!surfxr.observe_display_frame(1,1001,times[0],times[1],times[2]),"surfxr_invalid_or_missed_display_not_training");
        near(surfxr.statistics().latency_seconds,saved,"surfxr_rejections_preserve_estimate");
        surfxr.set_epoch(2);
        require(!surfxr.observe_display_frame(1,1001,1,38'000'001,60'000'001),"surfxr_old_epoch_after_recenter");
        require(surfxr.observe_display_frame(2,1,1,38'000'001,60'000'001),"surfxr_new_epoch_identity");
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        require(surfxr.observe_display_frame(2,2,maximum-60'000'000,maximum-22'000'000,maximum),"surfxr_subtract_before_float_conversion");
        near(surfxr.statistics().last_sample_seconds,.06,"surfxr_large_monotonic_epoch");
        for(std::uint64_t id=3;id<1000;++id)
            require(surfxr.observe_display_frame(2,id,1,38'000'001,90'000'001),"surfxr_slow_pipeline_sample");
        require(surfxr.target(1'000'000'000).xr_time==1'066'000'000,"surfxr_retains_prediction_safety_cap");
    }++groups;
    {using surfxr::motion_target_in_epoch;
        require(!motion_target_in_epoch(0,0,0)&&!motion_target_in_epoch(-1,0,0),"epoch_target_must_be_positive");
        require(motion_target_in_epoch(1,0,0),"unknown_epoch_bounds");
        require(!motion_target_in_epoch(99,100,200)&&motion_target_in_epoch(100,100,200),"epoch_start_is_inclusive");
        require(motion_target_in_epoch(199,100,200)&&!motion_target_in_epoch(200,100,200),"next_epoch_start_is_exclusive");
        require(!motion_target_in_epoch(201,100,200),"future_target_must_not_cross_reference_change");
        require(motion_target_in_epoch(200,200,0),"new_epoch_accepts_own_boundary");
        require(!motion_target_in_epoch(199,200,0),"old_remote_now_cannot_belong_to_new_local_display_epoch");
    }++groups;
    return groups;
}
}
int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cout<<"surfxr-motion-sync-test --self-test (CPU only)\n";return argc==1?0:1;}
    try{const auto groups=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"network_used\":false,\"decoder_used\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<"motion_sync_test: "<<e.what()<<'\n';return 1;}
}
