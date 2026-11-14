#include "surfxr/vr_client_state.hpp"
#include <stdexcept>

namespace surfxr {
void vr_surfxr_self_test(){
    const auto require=[](bool value){if(!value)throw std::runtime_error("surfxr_vr_adapter_check_failed");};
    constexpr std::uint64_t start=1'000'000'000,period=8'333'333,source=start-20'000'000;
    std::uint64_t clock=start;
    SessionControlQueue control;RemoteSettings settings;
    VrClientState state(control,settings,{120},true,8192,8192,[&]{return clock;});
    state.set_vr_only(true);state.connected(true);
    require(state.settings_snapshot().capabilities.vr_stream_framerate==0);
    require(state.statistics().requested_vr&&state.launch()&&!state.desktop()&&!state.exit_vr());
    require(!settings.snapshot().capabilities.server_video_controls&&settings.snapshot().capabilities.vr_only);
    require(!state.setting({SettingId::VrBitrateMbps,100}));
    require(state.setting({SettingId::MicrophoneEnabled,1})&&state.setting({SettingId::MicrophoneVolume,.5}));
    state.recenter();SessionControlCommand command;require(!control.pop(command));
    TrackingFrame tracking;tracking.sequence=1;tracking.origin_epoch=1;tracking.origin=TrackingOrigin::Stage;
    tracking.stage_supported=tracking.stage_locatable=tracking.optics_valid=true;
    tracking.head.flags=OrientationValid|PositionValid;tracking.ipd_meters=.064f;
    state.tracking(tracking);state.tracking_sent(tracking);
    VrConfiguration config;config.codec=2;config.frame_rate=120;config.stream_count=1;
    config.stereo_packed=true;config.render_width=config.render_height=32;config.streams.push_back({0,0,64,32,false});
    state.vr_configuration(config);require(state.statistics().playout_reserve_ns==2*period);
    require(state.settings_snapshot().capabilities.vr_stream_framerate==120);
    auto metadata=std::make_shared<VrVideoMetadata>();metadata->tracking_frame_id=1;
    metadata->pose_valid=metadata->fov_valid=metadata->linear_layout=true;
    const auto publish=[&](std::uint64_t index){
        auto m=std::make_shared<VrVideoMetadata>(*metadata);m->timestamp=source+index*period;
        const auto frame=[&]{auto f=std::make_shared<VideoFrame>();f->generation=index+1;
            f->has_source_timestamp=true;f->source_timestamp=m->timestamp;f->vr_metadata=m;return f;};
        state.decoded(0,frame());const auto previous=state.stereo();state.decoded(1,frame());
        require(state.stereo()!=previous&&state.stereo()->metadata==m);return state.stereo();
    };
    const auto first=publish(0);require(!state.presentation_stereo());
    clock=start+period;const auto second=publish(1);require(!state.presentation_stereo());
    clock=start+2*period;const auto third=publish(2);
    require(state.presentation_stereo()==first&&state.stereo()==third);
    state.stereo_presented(first);require(state.presentation_stereo()==first);
    require(state.statistics().paired_queue_depth==2);
    clock=start+3*period;require(state.presentation_stereo()==second);
    state.stereo_presented(second);require(state.presentation_stereo()==second);
    // A six-ms late arrival inside the reservoir keeps the original deadline.
    // Delaying every frame from its own arrival would repeat third at this slot.
    clock+=6'000'000;const auto fourth=publish(3);
    require(state.presentation_stereo()==second);
    clock=start+4*period;require(state.presentation_stereo()==third);state.stereo_presented(third);
    clock=start+5*period;require(state.presentation_stereo()==fourth);
    state.stereo_presented(fourth);require(state.presentation_stereo()==fourth);
    // A source stall must retain the actual submitted picture, without exposing
    // the newest decoder output early or presenting timestamps backwards.
    clock=start+6*period;const auto selected=publish(4);require(state.presentation_stereo()==selected);
    clock+=period;publish(5);clock+=period;const auto successor=publish(6);
    clock+=period;publish(7);clock+=period;const auto newest=publish(8);
    require(state.statistics().paired_queue_depth==3&&state.statistics().paired_queue_overflows==2);
    const auto before=state.statistics().queued_presentations;
    state.stereo_presented(std::make_shared<VrStereoFrame>(*selected));require(state.statistics().queued_presentations==before);
    state.stereo_presented(selected);state.stereo_presented(selected);
    require(state.statistics().queued_presentations==before+1&&state.statistics().paired_queue_depth==3);
    require(state.presentation_stereo()==successor);state.stereo_presented(successor);
    const auto latest_candidate=state.presentation_stereo();require(latest_candidate!=newest);
    state.stereo_presented(latest_candidate);require(state.presentation_stereo()==newest);state.stereo_presented(newest);
    clock+=1'000'000'000;require(state.presentation_stereo()==newest);
    // A resumed/configured stream has its own reservoir and identity; an old
    // ACK with a reused timestamp/epoch cannot revive it during initial fill.
    state.vr_configuration(config);require(!state.presentation_stereo());
    const auto reset=publish(8);state.stereo_presented(newest);require(!state.presentation_stereo());
    clock+=2*period;require(state.presentation_stereo()==reset);state.stereo_presented(reset);
    // A faster frame after a slow initial IDR moves the delivery floor forward,
    // instead of evicting not-yet-due pictures forever in the small reservoir.
    state.vr_configuration(config);const auto slow_initial=publish(8);require(!state.presentation_stereo());
    clock+=period;publish(12);require(state.presentation_stereo()==slow_initial);
    state.set_renderable(false);require(!state.presentation_stereo()&&state.statistics().paired_queue_depth==0);
    state.stereo_presented(reset);require(!state.presentation_stereo());
    state.connected(false);require(!state.stereo()&&!state.statistics().requested_vr);
    require(state.settings_snapshot().capabilities.vr_stream_framerate==0);
    state.connected(true);require(state.settings_snapshot().capabilities.vr_stream_framerate==0);
    state.set_surfxr_controls([&](SettingChange change){return settings.request(change);},[]{return true;});
    require(state.setting({SettingId::VrFramerate,144}));
    settings.synchronize_surfxr({120,4,200},{true,false,false});
    config.frame_rate=90;state.vr_configuration(config);
    require(state.settings_snapshot().capabilities.vr_stream_framerate==90);
    require(state.settings_snapshot().at(SettingId::VrFramerate).requested==144);
    state.source_config(0,2);require(state.settings_snapshot().capabilities.vr_stream_framerate==0);
    state.source_config(1,2);require(state.settings_snapshot().capabilities.vr_stream_framerate==0);
    state.vr_configuration(config);require(state.settings_snapshot().capabilities.vr_stream_framerate==90);
}
}
