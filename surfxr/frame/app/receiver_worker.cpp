#include "receiver_worker.hpp"
#include "surfxr/control_parser.hpp"
#include "surfxr/surfxr.hpp"
#include "surfxr/stereo_splitter.hpp"
#include "surfxr/surfxr_modes.hpp"
#include <cstdlib>
#include <string_view>

namespace surfxr::viewer {
void ReceiverWorker::run(){

    std::array<std::unique_ptr<surfxr::VideoDecoder>,4> videos;
    constexpr std::size_t vr_stream_capacity=4;
    std::array<surfxr::LatestFrameSlot,vr_stream_capacity> vr_slots;
    std::array<std::unique_ptr<surfxr::AsyncVideoDecoder>,vr_stream_capacity> vr_videos;
    unsigned vr_stream_count=0;
    bool vr_configured=false;
    const auto* desktop_nv12=std::getenv("SURFXR_DESKTOP_NV12");
    const bool desktop_gpu=!desktop_nv12||std::string_view(desktop_nv12)!="0";
    const auto* vr_nv12=std::getenv("SURFXR_VR_NV12");
    const bool vr_gpu=!vr_nv12||std::string_view(vr_nv12)!="0";
    surfxr::StereoSplitter stereo_splitter(opts.backend==surfxr::DecoderBackend::V4l2?display.rgba_frame_allocator():surfxr::RgbaFrameAllocator{});
    // Cadence belongs to each encoded stream, including both focus streams.
    std::array<surfxr::MediaCadence,vr_stream_capacity> vr_packet_cadence;
    std::chrono::steady_clock::time_point next_decode_sample{};
    const auto publish_decode_telemetry=[&](bool force=false){
        if(opts.status_path.empty())return;
        const auto now=std::chrono::steady_clock::now();
        if(!force&&now<next_decode_sample)return;
        next_decode_sample=now+std::chrono::seconds(2);
        surfxr::ViewerDecodeTelemetry snapshot;
        snapshot.sample_monotonic_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        for(unsigned kind=0;kind<2;++kind)for(unsigned id=0;id<vr_videos.size();++id){
            auto& value=kind?snapshot.desktop_decoders[id]:snapshot.vr_decoders[id];
            value.active=kind?available[id].load(std::memory_order_acquire):vr_configured&&id<vr_stream_count;
            if(kind?bool(videos[id]):bool(vr_videos[id])){
                const auto stats=kind?videos[id]->statistics():vr_videos[id]->statistics();
#define COPY_DECODER_TELEMETRY(name) value.name=stats.name
                COPY_DECODER_TELEMETRY(packets);COPY_DECODER_TELEMETRY(frames);COPY_DECODER_TELEMETRY(configuration_changes);COPY_DECODER_TELEMETRY(decode_time_ns);
                COPY_DECODER_TELEMETRY(avcodec_send_time_ns);COPY_DECODER_TELEMETRY(avcodec_receive_time_ns);COPY_DECODER_TELEMETRY(rgba_conversion_time_ns);
                COPY_DECODER_TELEMETRY(rgba_converted_frames);COPY_DECODER_TELEMETRY(rgba_skipped_frames);COPY_DECODER_TELEMETRY(rgba_suppressed_publications);
                COPY_DECODER_TELEMETRY(nv12_published_frames);COPY_DECODER_TELEMETRY(nv12_copy_time_ns);COPY_DECODER_TELEMETRY(nv12_pool_bytes);
                COPY_DECODER_TELEMETRY(nv12_zero_copy_frames);COPY_DECODER_TELEMETRY(nv12_zero_copy_time_ns);COPY_DECODER_TELEMETRY(nv12_zero_copy_fallback_frames);
                COPY_DECODER_TELEMETRY(rgba_buffer_allocations);COPY_DECODER_TELEMETRY(rgba_buffer_reuses);COPY_DECODER_TELEMETRY(rgba_temporary_allocations);COPY_DECODER_TELEMETRY(rgba_pool_bytes);
                COPY_DECODER_TELEMETRY(metadata_unmatched);COPY_DECODER_TELEMETRY(sequence_restarts);
                COPY_DECODER_TELEMETRY(async_queue_packets_max);COPY_DECODER_TELEMETRY(async_queue_bytes_max);
                COPY_DECODER_TELEMETRY(async_backpressure_wait_ns);COPY_DECODER_TELEMETRY(async_discarded_packets);
                COPY_DECODER_TELEMETRY(width);COPY_DECODER_TELEMETRY(height);COPY_DECODER_TELEMETRY(color_space);COPY_DECODER_TELEMETRY(color_range);
                COPY_DECODER_TELEMETRY(effective_color_space);COPY_DECODER_TELEMETRY(effective_color_range);
#undef COPY_DECODER_TELEMETRY
            }
            if(!kind){const auto& cadence=vr_packet_cadence[id].statistics();
                value.callback_packets=cadence.packets;value.packet_intervals=cadence.intervals;value.packet_interval_ns_sum=cadence.receive_interval_ns_sum;}
        }
        decode_telemetry.publish(snapshot);
    };
#ifdef SURFXR_HAVE_MICROPHONE
    std::unique_ptr<surfxr::MicrophoneCapture> microphone;
#endif
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
    std::unique_ptr<surfxr::AudioOutput> audio;
#endif
    std::atomic<std::uint64_t> received_pcm_bytes{0};
    std::chrono::steady_clock::time_point next_audio_sample{};
    const auto publish_audio_telemetry=[&](bool force=false){
        if(opts.status_path.empty())return;
        const auto now=std::chrono::steady_clock::now();
        if(!force&&now<next_audio_sample)return;
        next_audio_sample=now+std::chrono::seconds(1);
        surfxr::ViewerAudioTelemetry snapshot;snapshot.requested=opts.audio;
        snapshot.sample_monotonic_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        snapshot.received_pcm_bytes=received_pcm_bytes.load(std::memory_order_relaxed);
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
        snapshot.supported=true;if(audio)snapshot.stats=audio->statistics();
#endif
        result.runtime_status->audio_output(snapshot);
    };
    try{
        surfxr::SessionCallbacks callbacks;
        callbacks.steamvr_startup=[&](const auto& value){result.runtime_status->steamvr_startup(value);};
        callbacks.desktop_capture_mask=[&]{return desktop_demand.capture_requested.load();};
        callbacks.desktop_capture_ack=[&](bool supported,unsigned mask){desktop_demand.capture_accepted.store(mask);desktop_demand.capture_supported.store(supported);};
        if(opts.input){callbacks.desktop_input=[&](auto& event){return input->next_desktop_event(event);};callbacks.desktop_input_release=[&]{input->release_all();};}
#ifdef SURFXR_HAVE_MICROPHONE
        if(opts.session.microphone_enabled){
            callbacks.microphone_start=[&]{
                // SURFXR can reconnect in the same viewer. A stopped capture is
                // terminal; each attempt must own a fresh Pulse stream/ring.
                if(microphone)microphone->stop();
                microphone=std::make_unique<surfxr::MicrophoneCapture>();
                if(!microphone_enabled.load()){microphone.reset();return true;}
                try{microphone->start();result.microphone_error.clear();return true;}
                catch(const surfxr::MicrophoneCaptureError& error){result.microphone_error=error.what();return false;}
            };
            callbacks.microphone_pcm=[&](auto* data,auto size){
                try{
                    if(!microphone_enabled.load()){
                        if(microphone){microphone->stop();microphone.reset();}
                        return std::size_t{0};
                    }
                    if(!microphone){microphone=std::make_unique<surfxr::MicrophoneCapture>();microphone->start();result.microphone_error.clear();}
                    return microphone->read_pcm(data,size);
                }
                catch(const surfxr::MicrophoneCaptureError& error){result.microphone_error=error.what();throw;}
            };
            callbacks.microphone_stop=[&]{if(microphone)microphone->stop();};
            callbacks.microphone_volume=[&]{return static_cast<float>(settings.snapshot().at(surfxr::SettingId::MicrophoneVolume).requested);};
        }
#else
        if(opts.session.microphone_enabled)result.microphone_error="microphone_support_not_built";
#endif
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
        if(opts.audio){
            audio=std::make_unique<surfxr::AudioOutput>(surfxr::AudioPlaybackMode::Buffered);
            try{audio->initialize();}
            catch(const surfxr::AudioOutputError& error){result.audio_error=error.what();}
            // Connection readiness is asynchronous and may change after
            // an audio-server restart. Keep draining the transport while
            // the audio owner reconnects; never gate this callback on a
            // one-time initialization snapshot.
            callbacks.audio_pcm=[&](const auto* data,auto size){received_pcm_bytes.fetch_add(size,std::memory_order_relaxed);audio->submit_pcm(data,size);};
        }
#else
        if(opts.audio)result.audio_error="audio_support_not_built";
#endif
        publish_audio_telemetry(true);
        callbacks.desktop_config=[&](const surfxr::DesktopConfiguration& configuration){
            const bool retiring_vr=vr_configured;
            for(auto& video:vr_videos)if(video)video->deactivate();
            vr_configured=false;
            for(auto& cadence:vr_packet_cadence)cadence.configuration();
            publish_decode_telemetry(true);
            vr.source_config(0,configuration.streams.empty()?0:configuration.streams.front().codec);
            if(retiring_vr)display.retire_decoder_frames(stop_requested);
            vr.desktop_monitors(static_cast<unsigned>(configuration.streams.size()),configuration.monitor_capacity);
            std::array<bool,4> present{};
            for(const auto& stream:configuration.streams){
                if(stream.stream_id>=videos.size() || present[stream.stream_id])throw std::runtime_error("invalid_desktop_stream_id");
                present[stream.stream_id]=true;
            }
            // Release removed hardware contexts before opening new ones.
            // Keeping the decoder shell preserves monotonically growing
            // frame generations when a monitor is added again.
            for(std::size_t i=0;i<available.size();++i)if(!present[i]){
                available[i].store(false,std::memory_order_release);
                if(videos[i])videos[i]->deactivate();
                else slots[i].clear();
            }
            for(const auto& stream:configuration.streams){
                const auto id=stream.stream_id;
                // Bounded parallel color conversion keeps the media
                // receiver from serializing full-resolution CPU work.
                if(!videos[id])videos[id]=std::make_unique<surfxr::VideoDecoder>(slots[id],opts.backend,
                    [&,id](std::shared_ptr<const surfxr::VideoFrame>){vr.desktop_decoded(id);},opts.backend==surfxr::DecoderBackend::V4l2?4u:1u,
                    opts.backend==surfxr::DecoderBackend::V4l2?display.rgba_frame_allocator():surfxr::RgbaFrameAllocator{},
                    desktop_demand.enabled()?surfxr::VideoDecoder::FrameDemand{[&,id]{return desktop_demand.epoch(id);}}:surfxr::VideoDecoder::FrameDemand{},surfxr::DecoderFrameLayout::Single,
                    desktop_gpu&&opts.backend==surfxr::DecoderBackend::V4l2?display.nv12_frame_allocator():surfxr::Nv12FrameAllocator{});
                try{videos[id]->configure(stream.codec,stream.width,stream.height);}
                catch(const surfxr::VideoDecodeError& error){result.worker_error=error.what();throw;}
                available[id].store(true,std::memory_order_release);
            }
            publish_decode_telemetry(true);
        };
        callbacks.video_packet=[&](auto id,const auto* data,auto size,auto timestamp){
            if(id>=videos.size() || !videos[id] || !available[id].load())throw std::runtime_error("video_stream_not_configured");
            try{videos[id]->submit(data,size,timestamp);if(videos[id]->needs_keyframe()&&control->force_keyframe())videos[id]->keyframe_request_queued();}catch(const surfxr::VideoDecodeError& error){result.worker_error=error.what();throw;}
        };
        callbacks.vr_config=[&](const surfxr::VrConfiguration& configuration){
            try{
                const bool retiring=vr_configured||std::any_of(available.begin(),available.end(),[](const auto& value){return value.load();});
                // Finish old callbacks before changing their stream-to-eye
                // mapping or replacing the authoritative VR configuration.
                for(auto& video:vr_videos)if(video)video->deactivate();
                vr_configured=false;
                for(auto& cadence:vr_packet_cadence)cadence.configuration();
                vr.vr_configuration(configuration);
                for(std::size_t i=0;i<videos.size();++i){available[i].store(false,std::memory_order_release);if(videos[i])videos[i]->deactivate();}
                if(retiring)display.retire_decoder_frames(stop_requested);
                vr_stream_count=configuration.stream_count;
                std::array<bool,vr_stream_capacity> seen{};
                for(const auto& stream:configuration.streams){
                    if(stream.stream_id>=vr_stream_count||stream.eye!=(configuration.foveated?stream.stream_id/2:stream.stream_id)||
                       stream.focus_region!=(configuration.foveated&&stream.stream_id%2==1)||seen[stream.stream_id])throw std::runtime_error("invalid_vr_stream_id");
                    seen[stream.stream_id]=true;
                }
                for(unsigned id=0;id<vr_stream_count;++id)if(!seen[id])throw std::runtime_error("missing_vr_eye_stream");
                for(const auto& stream:configuration.streams){
                    const auto id=stream.stream_id;
                    if(!vr_videos[id])vr_videos[id]=std::make_unique<surfxr::AsyncVideoDecoder>(vr_slots[id],opts.backend,
                        [&,id](std::shared_ptr<const surfxr::VideoFrame> frame){
                            if(frame&&frame->vr_metadata&&frame->vr_metadata->gaze_atlas){const auto views=stereo_splitter.split_gaze(std::move(frame));for(unsigned i=0;i<4;++i)vr.decoded(i,views[i]);}
                            else{const auto eyes=stereo_splitter.split(std::move(frame));if(eyes[0]){vr.decoded(0,eyes[0]);vr.decoded(1,eyes[1]);}}
                            
                        },
                        opts.backend==surfxr::DecoderBackend::V4l2?4u:1u,
                        opts.backend==surfxr::DecoderBackend::V4l2?display.rgba_frame_allocator():surfxr::RgbaFrameAllocator{},
                        [&]{return vr.frame_demand_epoch();},
                        surfxr::DecoderFrameLayout::PackedStereo,
                        vr_gpu&&opts.backend==surfxr::DecoderBackend::V4l2?display.nv12_frame_allocator():surfxr::Nv12FrameAllocator{});
                    vr_videos[id]->configure(configuration.codec,stream.width,stream.height);
                }
                vr_configured=true;
                publish_decode_telemetry(true);
            }catch(const surfxr::VideoDecodeError& error){result.worker_error=error.what();throw;}
        };
        const auto receive_vr_packet=[&](auto id,const auto* data,auto size,auto timestamp,auto metadata,bool wait,
                                       std::vector<std::uint8_t>* owned=nullptr,std::size_t offset=0){
            try{
                if(!vr_configured||id>=vr_stream_count||!vr_videos[id])throw std::runtime_error("vr_stream_not_configured");
                if(opts.test_head_motion&&metadata&&metadata->pose_valid&&id==0)
                    test_head_motion.feedback(metadata->tracking_frame_id,metadata->timestamp,metadata->eye_poses[0].orientation);
                const auto now=std::chrono::steady_clock::now();
                vr_packet_cadence[id].observe(timestamp,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
                bool accepted=true;
                if(owned)accepted=vr_videos[id]->try_submit(std::move(*owned),offset,timestamp,std::move(metadata));
                else if(wait)vr_videos[id]->submit(data,size,timestamp,std::move(metadata));
                else accepted=vr_videos[id]->try_submit(data,size,timestamp,std::move(metadata));
                if(vr_videos[id]->needs_keyframe()&&control->force_keyframe())vr_videos[id]->keyframe_request_queued();
                publish_decode_telemetry();
                return accepted;
            }catch(const surfxr::VideoDecodeError& error){result.worker_error=error.what();throw;}
        };
        callbacks.vr_video_packet=[&](auto id,const auto* data,auto size,auto timestamp,auto metadata){receive_vr_packet(id,data,size,timestamp,std::move(metadata),true);};
        callbacks.try_vr_video_packet=[&](auto id,const auto* data,auto size,auto timestamp,auto metadata){return receive_vr_packet(id,data,size,timestamp,std::move(metadata),false);};
        callbacks.try_vr_video_packet_owned=[&](auto id,std::vector<std::uint8_t>&& bytes,std::size_t offset,auto timestamp,auto metadata){
            if(offset>=bytes.size())throw std::runtime_error("invalid_encoded_packet");
            return receive_vr_packet(id,bytes.data()+offset,bytes.size()-offset,timestamp,std::move(metadata),false,&bytes,offset);
        };
        callbacks.video_tick=[&]{
            try{
                // Stateful hardware can finish the last desktop packet after
                // submit() returns. A static desktop need not send another
                // packet; collect output while audio/control keep arriving.
                for(std::size_t id=0;id<videos.size();++id)if(videos[id]&&available[id].load(std::memory_order_acquire)){
                    videos[id]->poll();
                    if(videos[id]->needs_keyframe()&&control->force_keyframe())videos[id]->keyframe_request_queued();
                }
                for(auto& video:vr_videos)if(video){
                    video->check();
                    if(vr_configured&&video->needs_keyframe()&&control->force_keyframe())video->keyframe_request_queued();
                }
                publish_decode_telemetry();
            }catch(const surfxr::VideoDecodeError& error){result.worker_error=error.what();throw;}
            // Exhausted first-frame recovery closes the native channels through
            // the existing orderly session cleanup, instead of keeping an
            // audio-only connection alive and occupying the PC session.
            vr.tick();
            if(vr.statistics().desktop_failed){
                result.worker_error="desktop_switch_timed_out";
                throw surfxr::ParseError("desktop_switch_timed_out");
            }
            publish_audio_telemetry();
        };
        callbacks.show_desktop_requested=[&]{input->release_all();vr.desktop();};
        callbacks.tracking_frame=[&](auto& frame){const bool valid=vr.tracking_snapshot(frame);
            if(valid&&opts.test_head_motion)test_head_motion.apply(frame);
            frame.battery=battery_monitor.snapshot();return valid;};
        callbacks.sample_tracking=[&](auto& frame){const bool valid=display.sample_tracking(frame);
            if(valid&&opts.test_head_motion)test_head_motion.apply(frame);
            frame.battery=battery_monitor.snapshot();return valid;};
        callbacks.timestamped_tracking=[&](bool enabled){display.set_timestamped_tracking(enabled);};
        callbacks.gaze_sample=[&](auto& sample){surfxr::TrackingFrame frame;
            if(!vr.tracking_snapshot(frame))return false;
            sample=frame.gaze;
            return frame.focused&&frame.visible&&sample.origin_epoch==frame.origin_epoch;
        };
        callbacks.vr_controllers_enabled=[&]{return controllers_enabled.load(std::memory_order_relaxed);};
        callbacks.tracking_sent=[&](const auto& frame){vr.tracking_sent(frame);if(opts.test_head_motion)test_head_motion.sent(frame);};
        if(!opts.status_path.empty())callbacks.tracking_telemetry=[&](const auto& value){result.runtime_status->tracking_upload(value);};
        callbacks.connection_started=[&]{
            if(opts.session.surfxr_modes){
                const bool desktop=opts.session.surfxr_modes->desktop();
                vr.set_vr_only(!desktop);vr.set_desktop_only(desktop);
            }
            result.connection_started.store(true,std::memory_order_relaxed);vr.connected(true);
        };
        callbacks.connection_ended=[&]{
            for(auto& video:vr_videos)if(video)video->deactivate();
            for(std::size_t id=0;id<videos.size();++id){available[id].store(false,std::memory_order_release);if(videos[id])videos[id]->deactivate();else slots[id].clear();}
            vr_configured=false;vr.connected(false);publish_decode_telemetry(true);
            display.retire_decoder_frames(stop_requested);
        };
        callbacks.control_command_sent=[&](const auto& command){vr.control_command_sent(command);};
        result.receiver=(opts.session.surfxr_modes ? surfxr::transport::run_modes(opts.session,callbacks,&stop_requested) : opts.session.surfxr_desktop ? surfxr::transport::run_desktop(opts.session,callbacks,&stop_requested) : surfxr::transport::run(opts.session,callbacks,&stop_requested));
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
        if(audio)audio->stop();
#endif
        if(result.receiver.exit_code!=1)for(auto& video:videos)if(video && !video->statistics().failed)video->finish();
        result.worker_failed=result.receiver.exit_code==1;
    }catch(const surfxr::VideoDecodeError& error){result.worker_failed=true;result.worker_error=error.what();}
    catch(...){result.worker_failed=true;result.worker_error="worker_failed";}
    // Join decoder callbacks while all captured worker
    // locals still exist. Published Vulkan storage outlives these
    // workers and is retired by XR before the device is destroyed.
    for(auto& video:vr_videos)if(video)video->stop();
    for(std::size_t i=0;i<videos.size();++i)if(videos[i]){result.decoders[i]=videos[i]->statistics();result.worker_failed=result.worker_failed || result.decoders[i].failed;}
    for(std::size_t i=0;i<vr_videos.size();++i)if(vr_videos[i]){result.vr_decoders[i]=vr_videos[i]->statistics();result.worker_failed=result.worker_failed||result.vr_decoders[i].failed;}
    vr_configured=false;publish_decode_telemetry(true);
    vr.connected(false);
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
    if(audio){audio->stop();result.audio_report=audio_json(audio->statistics());}
#endif
    publish_audio_telemetry(true);
    #ifdef SURFXR_HAVE_MICROPHONE
    if(microphone){microphone->stop();result.microphone_report=microphone_json(microphone->statistics());}
#endif
    result.worker_completed=true;
    stop_requested.store(true,std::memory_order_relaxed);

}
}
