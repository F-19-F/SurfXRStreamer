// Offline packet-interface compatibility check. libavformat is used ONLY here
// to recover access-unit boundaries from an explicitly provided local fixture.
#include "surfxr/video_decoder.hpp"
#include "surfxr/vr_wire.hpp"
#include "color_bars_check.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/hash.h>
#include <libavutil/log.h>
}
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef SURFXR_NV12_COPY_BENCHMARK
int nv12_copy_benchmark(const std::string&,unsigned,unsigned);
void nv12_copy_candidates_self_test();
#endif

namespace {
void require(bool value,const char* message){if(!value)throw surfxr::VideoDecodeError(message);}
std::uint64_t monotonic_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Format {AVFormatContext* value=nullptr;~Format(){avformat_close_input(&value);}};
struct Packet {AVPacket* value=av_packet_alloc();~Packet(){av_packet_free(&value);}};
struct ExternalTestStorage final:surfxr::RgbaFrameStorage {
    explicit ExternalTestStorage(std::size_t bytes):pixels(bytes){}
    std::uint8_t* data()noexcept override{return pixels.data();}
    std::size_t size()const noexcept override{return pixels.size();}
    std::vector<std::uint8_t> pixels;
};
std::uint32_t number(const char* value){
    std::uint32_t result=0;const auto end=value+std::strlen(value);const auto p=std::from_chars(value,end,result);
    require(p.ec==std::errc{} && p.ptr==end,"invalid_integer");return result;
}
surfxr::DecoderBackend backend(const char* value){
    if(std::strcmp(value,"software")==0)return surfxr::DecoderBackend::Software;
    if(std::strcmp(value,"v4l2")==0)return surfxr::DecoderBackend::V4l2;
    throw surfxr::VideoDecodeError("invalid_decoder_backend");
}
std::string hash(const surfxr::VideoFrame& frame){
    AVHashContext* raw=nullptr;require(av_hash_alloc(&raw,"sha256")==0,"hash_init_failed");
    struct Cleanup{AVHashContext* value;~Cleanup(){av_hash_freep(&value);}} cleanup{raw};
    av_hash_init(raw);av_hash_update(raw,frame.rgba_data(),frame.rgba_size());
    std::array<std::uint8_t,65> text{};av_hash_final_hex(raw,text.data(),text.size());return reinterpret_cast<const char*>(text.data());
}
}
int main(int argc,char** argv){
    av_log_set_level(AV_LOG_QUIET);
    surfxr::DecoderBackend selected_backend=surfxr::DecoderBackend::Software;
    const char* phase="arguments";std::uint64_t phase_packet=0;surfxr::DecoderStats failure_stats;
    try{
        if(argc==2 && std::strcmp(argv[1],"--self-test")==0){
#ifdef SURFXR_NV12_COPY_BENCHMARK
            nv12_copy_candidates_self_test();
#endif
            surfxr::video_decoder_self_test();
            require(backend("software")==surfxr::DecoderBackend::Software && backend("v4l2")==surfxr::DecoderBackend::V4l2,"backend_parse_failed");
            bool rejected=false;try{backend("auto");}catch(const surfxr::VideoDecodeError&){rejected=true;}
            require(rejected,"backend_parse_failed");
            std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"hardware_devices_used\":false}\n";return 0;
        }
        if(argc==2 && std::strcmp(argv[1],"--help")==0){
            std::cout<<"surfxr-video-test --input FILE --format h264|hevc|obu --codec NUMBER --width W --height H [--backend software|v4l2] [--rgba-threads 1|4] [--rgba-storage external-test] [--deactivate-test on] [--metadata-test on|delayed] [--poll-test on] [--expected-restarts N] [--color-bars on]\n"
                <<"Explicit v4l2 selects H264 (1) or HEVC8 (2), with synchronous NV12-to-RGBA output and no software fallback.\n"
                <<"Color bars checks the shared 1024x256 synthetic fixture against RGB values, with an eight-code-value tolerance and no pixel export.\n"
                <<"Metadata tests verify distinct per-packet VR poses; delayed also requires reordered output and frames produced during finish.\n"
                <<"Poll test waits 1ms between ten idle receives per packet (at most120packets); hardware must produce output without another submit.\n"
#ifdef SURFXR_NV12_COPY_BENCHMARK
                <<"--nv12-copy-benchmark on compares HEVC CAPTURE copies into six Vulkan NV12 images; requires explicit v4l2, an idle viewer and a local synthetic fixture. Does not measure GPU sampling or start XR.\n"
#endif
                <<"V4L2 library calls can block; run hardware fixtures under an external process-group deadline.\n";return 0;
        }
        std::string input,format;std::uint32_t codec=0,width=0,height=0;
        bool backend_set=false,deactivate_test=false,metadata_test=false,require_delayed_metadata=false,poll_test=false,nv12_benchmark=false;
        unsigned rgba_threads=1;bool rgba_threads_set=false;
        bool external_storage=false;
        bool color_bars=false;surfxr::tests::ColorBarsCheck color_check;
        int expected_restarts=-1;
        for(int i=1;i<argc;++i){
            require(i+1<argc,"missing_option_value");const std::string option=argv[i];const char* value=argv[++i];
            if(option=="--input")input=value;
            else if(option=="--format")format=value;
            else if(option=="--codec")codec=number(value);
            else if(option=="--width")width=number(value);
            else if(option=="--height")height=number(value);
            else if(option=="--backend"){
                require(!backend_set,"duplicate_backend_option");
                selected_backend=backend(value);backend_set=true;
            }
            else if(option=="--rgba-threads"){
                require(!rgba_threads_set,"duplicate_rgba_threads_option");rgba_threads=number(value);rgba_threads_set=true;
                require(rgba_threads==1||rgba_threads==4,"invalid_rgba_threads");
            }
            else if(option=="--rgba-storage"){
                require(!external_storage&&std::strcmp(value,"external-test")==0,"invalid_rgba_storage_option");external_storage=true;
            }
            else if(option=="--color-bars"){
                require(!color_bars&&std::strcmp(value,"on")==0,"invalid_color_bars_option");color_bars=true;
            }
            else if(option=="--deactivate-test"){
                require(!deactivate_test && std::strcmp(value,"on")==0,"invalid_deactivate_test_option");deactivate_test=true;
            }
            else if(option=="--metadata-test"){
                require(!metadata_test&&(std::strcmp(value,"on")==0||std::strcmp(value,"delayed")==0),"invalid_metadata_test_option");
                metadata_test=true;require_delayed_metadata=std::strcmp(value,"delayed")==0;
            }
            else if(option=="--poll-test"){
                require(!poll_test&&std::strcmp(value,"on")==0,"invalid_poll_test_option");poll_test=true;
            }
#ifdef SURFXR_NV12_COPY_BENCHMARK
            else if(option=="--nv12-copy-benchmark"){
                require(!nv12_benchmark&&std::strcmp(value,"on")==0,"invalid_nv12_benchmark_option");nv12_benchmark=true;
            }
#endif
            else if(option=="--expected-restarts"){
                const auto parsed=number(value);require(expected_restarts<0&&parsed<=8,"invalid_expected_restarts");expected_restarts=static_cast<int>(parsed);
            }
            else throw surfxr::VideoDecodeError("unknown_option");
        }
        require(format=="h264" || format=="hevc" || format=="obu","unsupported_fixture_format");
        require(!color_bars||(width==surfxr::tests::color_bars::width&&height==surfxr::tests::color_bars::height&&!nv12_benchmark),
                "color_bars_requires_shared_fixture_dimensions");
        if(selected_backend==surfxr::DecoderBackend::V4l2)
            require((codec==2&&format=="hevc")||(codec==1&&format=="h264"),"v4l2_fixture_requires_h264_or_hevc8");
        std::error_code error;require(std::filesystem::is_regular_file(input,error) && !error,"fixture_not_regular_file");
        const auto size=std::filesystem::file_size(input,error);require(!error && size>0 && size<=32u*1024u*1024u,"fixture_size_out_of_range");
#ifdef SURFXR_NV12_COPY_BENCHMARK
        if(nv12_benchmark){require(selected_backend==surfxr::DecoderBackend::V4l2&&codec==2&&format=="hevc"&&
            !poll_test&&!metadata_test&&!deactivate_test&&!external_storage&&!rgba_threads_set&&expected_restarts<0&&
            width>=128&&height>=128,"nv12_benchmark_requires_hevc_hardware_fixture");
            return nv12_copy_benchmark(input,width,height);}
#else
        (void)nv12_benchmark;
#endif
        phase="open_fixture";
        Format stream;AVDictionary* options=nullptr;av_dict_set(&options,"protocol_whitelist","file",0);
        const auto* demuxer=av_find_input_format(format.c_str());require(demuxer,"fixture_demuxer_missing");
        const auto opened=avformat_open_input(&stream.value,input.c_str(),demuxer,&options);av_dict_free(&options);
        require(opened>=0,"fixture_open_failed");
        surfxr::LatestFrameSlot slot;
        std::uint64_t packets=0,observed_generations=0,last_generation=0;bool saw_before_flush=false;
        std::uint64_t callback_frames=0,callback_multi_frame_submits=0,callback_max_frames_per_submit=0;
        std::uint64_t poll_calls=0,poll_frames=0,poll_submits=0;
        std::uint64_t poll_call_ns_max=0;
        std::shared_ptr<const surfxr::VideoFrame> lifetime_snapshot;
        std::shared_ptr<const surfxr::VrVideoMetadata> lifetime_metadata;
        std::string lifetime_hash;std::uint64_t lifetime_generation=0,lifetime_timestamp=0;
        std::array<unsigned,9> sequence_observations{};
        struct ExpectedMetadata {
            std::uint64_t timestamp;std::weak_ptr<const surfxr::VrVideoMetadata> identity;
            std::uint64_t submit_begin_ns=0,submit_end_ns=0;
        };
        std::vector<ExpectedMetadata> expected_metadata;
        std::uint64_t metadata_observations=0,metadata_delayed_observations=0,metadata_last_generation=0,latest_submitted_timestamp=0;
        const auto metadata_for=[&](std::uint64_t timestamp)->std::shared_ptr<const surfxr::VrVideoMetadata>{
            if(!metadata_test)return {};
            auto metadata=std::make_shared<surfxr::VrVideoMetadata>();metadata->timestamp=timestamp;
            metadata->tracking_frame_id=static_cast<std::int32_t>(100000+17*(timestamp/1000));
            metadata->header_present=metadata->pose_valid=metadata->fov_valid=true;
            metadata->eye_poses[0].position.x=static_cast<float>(timestamp/1000)*.001f;
            metadata->eye_poses[1].position.x=metadata->eye_poses[0].position.x+.064f;
            for(auto& fov:metadata->fov)fov={1.0f,1.1f,1.2f,1.3f};
            expected_metadata.push_back({timestamp,metadata});return metadata;
        };
        const auto verify_metadata=[&](const std::shared_ptr<const surfxr::VideoFrame>& frame){
            if(!metadata_test||!frame||frame->generation==metadata_last_generation)return;
            require(frame->has_source_timestamp&&frame->vr_metadata,"decoded_vr_metadata_missing");
            const auto found=std::find_if(expected_metadata.begin(),expected_metadata.end(),[&](const auto& expected){return expected.timestamp==frame->source_timestamp;});
            require(found!=expected_metadata.end(),"decoded_vr_metadata_unknown_timestamp");
            const auto identity=found->identity.lock();const auto& metadata=*frame->vr_metadata;
            require(identity&&identity.get()==frame->vr_metadata.get(),"decoded_vr_metadata_wrong_packet_identity");
            require(frame->packet_submit_monotonic_ns>=found->submit_begin_ns&&
                    frame->packet_submit_monotonic_ns<=(found->submit_end_ns?found->submit_end_ns:monotonic_ns()),
                    "decoded_packet_submit_timing_wrong_packet_identity");
            require(metadata.timestamp==frame->source_timestamp&&metadata.tracking_frame_id==static_cast<std::int32_t>(100000+17*(frame->source_timestamp/1000))&&
                    metadata.eye_poses[0].position.x==static_cast<float>(frame->source_timestamp/1000)*.001f&&metadata.header_present&&metadata.pose_valid&&metadata.fov_valid,"decoded_vr_metadata_mismatched_pose");
            ++metadata_observations;if(frame->source_timestamp!=latest_submitted_timestamp)++metadata_delayed_observations;
            metadata_last_generation=frame->generation;
        };
        surfxr::VideoDecoder decoder(slot,selected_backend,[&](std::shared_ptr<const surfxr::VideoFrame> frame){
            require(frame&&frame->generation==callback_frames+1,"frame_sink_skipped_or_reordered_generation");
            require(frame->conversion_begin_monotonic_ns&&frame->conversion_begin_monotonic_ns<=frame->decoded_monotonic_ns,
                    "decoded_conversion_timing_invalid");
            if(frame->has_source_timestamp)require(frame->packet_submit_monotonic_ns&&
                    frame->packet_submit_monotonic_ns<=frame->conversion_begin_monotonic_ns,"decoded_submit_timing_invalid");
            if(external_storage)require(frame->rgba_storage&&frame->rgba.empty(),"frame_sink_lost_external_storage");
            require(slot.snapshot().get()==frame.get(),"frame_sink_slot_identity_mismatch");
            if(!lifetime_snapshot){
                lifetime_snapshot=frame;lifetime_hash=hash(*frame);lifetime_generation=frame->generation;
                lifetime_timestamp=frame->source_timestamp;lifetime_metadata=frame->vr_metadata;
            }else require(lifetime_snapshot->generation==lifetime_generation&&lifetime_snapshot->source_timestamp==lifetime_timestamp
                    &&lifetime_snapshot->vr_metadata==lifetime_metadata,"rgba_pool_overwrote_held_packet_identity");
            verify_metadata(frame);++callback_frames;
            if(color_bars)color_check.observe(*frame);
        },rgba_threads,external_storage?surfxr::RgbaFrameAllocator([](std::size_t bytes){return std::make_shared<ExternalTestStorage>(bytes);}):surfxr::RgbaFrameAllocator{});
        struct SaveDiagnostics {surfxr::VideoDecoder& decoder;surfxr::DecoderStats& result;~SaveDiagnostics(){result=decoder.statistics();}} diagnostics{decoder,failure_stats};
        phase="configure";decoder.configure(codec,width,height);
        Packet packet;require(packet.value,"packet_allocation_failed");
        const auto submit_fixture=[&](const std::uint8_t* data,std::size_t size){
            if(poll_test)require(++poll_submits<=120,"poll_fixture_packet_limit");
            const auto before=callback_frames;
            auto metadata=metadata_for(latest_submitted_timestamp);
            if(metadata_test)expected_metadata.back().submit_begin_ns=monotonic_ns();
            // Exact-size caller storage may be reused immediately after submit,
            // even when the device/software codec publishes only during poll.
            std::vector<std::uint8_t> input(data,data+size);
            decoder.submit(input.data(),input.size(),latest_submitted_timestamp,std::move(metadata));
            std::fill(input.begin(),input.end(),0xcc);
            if(metadata_test)expected_metadata.back().submit_end_ns=monotonic_ns();
            const auto produced=callback_frames-before;
            callback_max_frames_per_submit=std::max(callback_max_frames_per_submit,produced);
            if(produced>1)++callback_multi_frame_submits;
            require(callback_frames==decoder.statistics().frames,"frame_sink_missed_submit_output");
            if(poll_test)for(unsigned attempt=0;attempt<10;++attempt){
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const auto stats_before=decoder.statistics();const auto callback_before=callback_frames;
                const auto poll_start=monotonic_ns();const auto received=decoder.poll();
                poll_call_ns_max=std::max(poll_call_ns_max,monotonic_ns()-poll_start);
                ++poll_calls;poll_frames+=received;
                require(decoder.statistics().packets==stats_before.packets&&
                        decoder.statistics().frames==stats_before.frames+received&&callback_frames==callback_before+received,
                        "idle_poll_submitted_packet_or_missed_output");
            }
        };
        std::vector<std::vector<std::uint8_t>> replay;std::size_t replay_bytes=0;
        while(true){
            const auto read=av_read_frame(stream.value,packet.value);
            if(read==AVERROR_EOF)break;
            require(read>=0 && packet.value->size>0 && packet.value->stream_index==0,"fixture_packet_failed");
            require(++packets<=300,"fixture_packet_limit");
            if(deactivate_test||(require_delayed_metadata&&selected_backend==surfxr::DecoderBackend::Software)){
                replay_bytes+=static_cast<std::size_t>(packet.value->size);require(replay_bytes<=32u*1024u*1024u,"fixture_replay_size_limit");
                replay.emplace_back(packet.value->data,packet.value->data+packet.value->size);
            }
            latest_submitted_timestamp=packets*1000;
            phase="initial_submit";phase_packet=packets;
            submit_fixture(packet.value->data,static_cast<std::size_t>(packet.value->size));
            av_packet_unref(packet.value);
            if(const auto latest=slot.snapshot()){
                verify_metadata(latest);
                require(latest->generation>=last_generation,"generation_regressed");
                if(latest->generation>last_generation){++observed_generations;
                    const auto sequence=decoder.statistics().sequence_restarts;
                    if(sequence<sequence_observations.size())++sequence_observations[sequence];}
                last_generation=latest->generation;saw_before_flush=true;
            }
        }
        std::uint64_t frames_before_deactivation=0;
        if(deactivate_test){
            const auto before=decoder.statistics();const auto held=slot.snapshot();
            require(held && before.frames>0 && held->generation==before.frames,"deactivate_fixture_requires_unflushed_output");
            const auto held_hash=hash(*held);frames_before_deactivation=before.frames;
            phase="deactivate";phase_packet=0;
            decoder.deactivate();decoder.deactivate();const auto after=decoder.statistics();
            require(!slot.snapshot() && after.packets==before.packets && after.frames==before.frames &&
                    after.configuration_changes==before.configuration_changes && after.decode_time_ns==before.decode_time_ns &&
                    after.rgba_conversion_time_ns==before.rgba_conversion_time_ns && !after.failed,"deactivate_statistics_or_slot_failed");
            require(held->generation==before.frames && hash(*held)==held_hash,"deactivate_invalidated_held_snapshot");
            phase="reconfigure";decoder.configure(codec,width,height);require(decoder.statistics().configuration_changes==before.configuration_changes+1,"deactivate_reconfigure_failed");
            std::uint64_t index=0;
            for(const auto& encoded:replay){
                ++index;latest_submitted_timestamp=(metadata_test?packets+index:index)*1000;
                phase="replay_submit";phase_packet=index;
                submit_fixture(encoded.data(),encoded.size());
                if(const auto frame=slot.snapshot()){
                    verify_metadata(frame);
                    require(frame->generation>before.frames && frame->generation>=last_generation,"deactivate_generation_regressed");
                    if(frame->generation>last_generation)++observed_generations;
                    last_generation=frame->generation;
                }
            }
        }
        const auto frames_before_flush=decoder.statistics().frames;
        const auto callback_frames_before_flush=callback_frames;
        phase="finish";phase_packet=0;
        decoder.finish();const auto stats=decoder.statistics();const auto latest=slot.snapshot();
        phase="validate_finished_frame";
        verify_metadata(latest);
        require(latest && stats.frames>0 && !stats.failed,"fixture_no_frames");
        require(lifetime_snapshot&&hash(*lifetime_snapshot)==lifetime_hash&&lifetime_snapshot->generation==lifetime_generation
                &&lifetime_snapshot->vr_metadata==lifetime_metadata,"rgba_pool_overwrote_held_pixels_or_metadata");
        require(stats.rgba_buffer_allocations+stats.rgba_buffer_reuses==stats.frames&&stats.rgba_pool_buffers<=6
                &&stats.rgba_pool_bytes<=128u*1024u*1024u,"rgba_pool_frame_accounting_or_bound_failed");
        require(callback_frames==stats.frames&&callback_frames-callback_frames_before_flush==stats.frames-frames_before_flush,"frame_sink_missed_flush_output");
        require(latest->generation==stats.frames && latest->rgba_size()==std::size_t(latest->stride)*latest->height,"latest_frame_layout_invalid");
        const bool timestamp_preserved=latest->has_source_timestamp && latest->source_timestamp>=1000 && latest->source_timestamp<=latest_submitted_timestamp;
        if(selected_backend==surfxr::DecoderBackend::Software || latest->has_source_timestamp)
            require(timestamp_preserved,"packet_timestamp_not_preserved");
        if(metadata_test)require(metadata_observations==stats.frames&&stats.metadata_unmatched==0,"metadata_fixture_missing_per_frame_observations");
        if(poll_test&&selected_backend==surfxr::DecoderBackend::V4l2)require(poll_frames>0,"hardware_poll_did_not_produce_output");
        if(require_delayed_metadata)require(metadata_delayed_observations>0&&stats.frames>frames_before_flush,"metadata_fixture_requires_delayed_and_flush_output");
        if(expected_restarts>=0){
            require(stats.sequence_restarts==static_cast<unsigned>(expected_restarts),"sequence_restart_count_mismatch");
            for(int i=0;i<=expected_restarts;++i)require(sequence_observations[i]>0,"sequence_did_not_produce_frames");
            require(stats.metadata_unmatched==0,"sequence_restart_lost_metadata");
        }
        // The delayed software fixture produces multiple frames during finish,
        // and also exercises a real decoded-frame callback exception. Do not
        // open a second hardware context merely to test exception propagation.
        bool callback_error_test=false;
        if(require_delayed_metadata&&selected_backend==surfxr::DecoderBackend::Software){
            require(callback_frames-callback_frames_before_flush>=2,"frame_sink_fixture_requires_multi_frame_flush");
            surfxr::LatestFrameSlot throwing_slot;std::uint64_t throw_calls=0;
            surfxr::VideoDecoder throwing(throwing_slot,surfxr::DecoderBackend::Software,
                [&](std::shared_ptr<const surfxr::VideoFrame>){++throw_calls;throw std::runtime_error("fixture_sink_rejected");},rgba_threads);
            throwing.configure(codec,width,height);
            bool rejected=false;
            try{std::uint64_t timestamp=0;for(const auto& encoded:replay)throwing.submit(encoded.data(),encoded.size(),++timestamp);
                throwing.finish();}
            catch(const std::runtime_error& error){rejected=std::strcmp(error.what(),"fixture_sink_rejected")==0;}
            require(rejected&&throw_calls==1&&throwing.statistics().failed,"frame_sink_exception_not_terminal");
            bool guarded=false;try{throwing.configure(codec,width,height);}
            catch(const surfxr::VideoDecodeError& error){guarded=std::strcmp(error.what(),"decoder_failed_state")==0;}
            require(guarded,"frame_sink_failure_allowed_reuse");callback_error_test=true;
        }
        std::cout<<"{\"status\":\""<<(color_bars&&!color_check.passed()?"color_mismatch":"decoded")
            <<"\",\"network_used\":false,\"packets\":"<<packets<<",\"decoded_frames\":"<<stats.frames
            <<",\"deactivation_test\":"<<(deactivate_test?"true":"false")<<",\"frames_before_deactivation\":"<<frames_before_deactivation
            <<",\"configuration_changes\":"<<stats.configuration_changes<<",\"submitted_packets\":"<<stats.packets
            <<",\"sequence_changes\":"<<stats.sequence_changes<<",\"sequence_restarts\":"<<stats.sequence_restarts
            <<",\"waiting_for_keyframe_packets\":"<<stats.waiting_for_keyframe_packets
            <<",\"backend\":\""<<surfxr::decoder_backend_name(stats.backend)<<"\",\"selected_decoder\":\""<<stats.selected_decoder
            <<"\",\"pixel_format\":\""<<stats.pixel_format<<"\",\"decode_time_ns\":"<<stats.decode_time_ns
            <<",\"rgba_conversion_time_ns\":"<<stats.rgba_conversion_time_ns
            <<",\"rgba_conversion_threads\":"<<stats.rgba_conversion_threads
            <<",\"color_space\":"<<stats.color_space<<",\"color_range\":"<<stats.color_range
            <<",\"effective_color_space\":"<<stats.effective_color_space<<",\"effective_color_range\":"<<stats.effective_color_range
            <<",\"external_rgba_storage_test\":"<<(external_storage?"true":"false")
            <<",\"rgba_buffer_allocations\":"<<stats.rgba_buffer_allocations<<",\"rgba_buffer_reuses\":"<<stats.rgba_buffer_reuses
            <<",\"rgba_temporary_allocations\":"<<stats.rgba_temporary_allocations<<",\"rgba_pool_buffers\":"<<stats.rgba_pool_buffers
            <<",\"rgba_pool_bytes\":"<<stats.rgba_pool_bytes<<",\"held_frame_lifetime_verified\":true"
            <<",\"metadata_from_opaque\":"<<stats.metadata_from_opaque<<",\"metadata_from_pts\":"<<stats.metadata_from_pts<<",\"metadata_unmatched\":"<<stats.metadata_unmatched
            <<",\"frames_before_flush\":"<<(saw_before_flush?"true":"false")<<",\"observed_generations\":"<<observed_generations
            <<",\"frames_produced_by_flush\":"<<(stats.frames-frames_before_flush)<<",\"metadata_test\":"<<(metadata_test?"true":"false")
            <<",\"callback_frames\":"<<callback_frames<<",\"callback_multi_frame_submits\":"<<callback_multi_frame_submits
            <<",\"callback_max_frames_per_submit\":"<<callback_max_frames_per_submit<<",\"callback_frames_from_flush\":"<<(callback_frames-callback_frames_before_flush)
            <<",\"callback_error_test\":"<<(callback_error_test?"true":"false")
            <<",\"poll_test\":"<<(poll_test?"true":"false")<<",\"poll_calls\":"<<poll_calls<<",\"poll_frames\":"<<poll_frames
            <<",\"poll_call_ns_max\":"<<poll_call_ns_max
            <<",\"metadata_observations\":"<<metadata_observations<<",\"metadata_delayed_observations\":"<<metadata_delayed_observations
            <<",\"latest_generation\":"<<latest->generation<<",\"width\":"<<latest->width<<",\"height\":"<<latest->height
            <<",\"rgba_bytes\":"<<latest->rgba_size()<<",\"source_timestamp_preserved\":"<<(timestamp_preserved?"true":"false")
            <<",\"latest_rgba_sha256\":\""<<hash(*latest)<<"\"";
        if(color_bars)color_check.report(std::cout);
        std::cout<<"}\n";
        return color_bars&&!color_check.passed()?1:0;
    }catch(const surfxr::VideoDecodeError& error){
        std::cerr<<"{\"status\":\"decode_failed\",\"backend\":\""<<surfxr::decoder_backend_name(selected_backend)
            <<"\",\"stage\":\""<<error.what()<<"\",\"ffmpeg_code\":"<<error.ffmpeg_code
            <<",\"phase\":\""<<phase<<"\",\"phase_packet\":"<<phase_packet<<",\"processed_packets\":"<<failure_stats.packets<<",\"decoded_frames\":"<<failure_stats.frames
            <<",\"metadata_from_opaque\":"<<failure_stats.metadata_from_opaque<<",\"metadata_from_pts\":"<<failure_stats.metadata_from_pts<<",\"metadata_unmatched\":"<<failure_stats.metadata_unmatched<<"}\n";return 1;
    }catch(...){std::cerr<<"{\"status\":\"decode_failed\",\"stage\":\"resource_failure\"}\n";return 1;}
}
