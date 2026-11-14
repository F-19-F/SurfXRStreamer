#include "viewer_affinity_checks.hpp"
#include "viewer_command_checks.hpp"
#include "viewer_startup_checks.hpp"
#include "surfxr/viewer_status.hpp"
#include "surfxr/network_binding.hpp"
#include "surfxr/cpu_affinity.hpp"
#include <iostream>
#include <sys/stat.h>
#include <vector>
#include <atomic>
#include <algorithm>
#include <limits>

using namespace surfxr::tests;
int main(int argc,char** argv){
    if(argc!=2||std::string(argv[1])!="--self-test")return 1;
    char directory[]="/tmp/surfxr-status-test-XXXXXX";
    if(!mkdtemp(directory))return 1;
    const std::string path=std::string(directory)+"/state.json";
    int result=0;
    try{
        surfxr::tests::viewer_affinity_checks();
        require(surfxr::valid_local_ipv4("10.35.78.1")&&surfxr::valid_local_ipv4("127.0.0.1")&&
                !surfxr::valid_local_ipv4("0.0.0.0")&&!surfxr::valid_local_ipv4("224.0.0.1")&&!surfxr::valid_local_ipv4("invalid"),"invalid_local_bind_validation");
        const int bound=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);require(bound>=0,"bind_fixture_socket_failed");
        require(surfxr::bind_local_ipv4(bound,"127.0.0.1"),"loopback_binding_failed");
        sockaddr_in actual{};socklen_t actual_size=sizeof(actual);
        const bool local_ok=getsockname(bound,reinterpret_cast<sockaddr*>(&actual),&actual_size)==0&&ntohl(actual.sin_addr.s_addr)==0x7f000001;
        close(bound);require(local_ok,"selected_local_address_not_bound");
        require(!surfxr::bind_local_ipv4(-1,"127.0.0.1")&&!surfxr::bind_local_ipv4(-1,"0.0.0.0"),"binding_failure_must_not_fallback");
        {surfxr::ViewerInstanceLock first(directory);bool rejected=false;
            try{surfxr::ViewerInstanceLock second(directory);}catch(...){rejected=true;}
            require(rejected,"viewer_duplicate_instance_not_rejected");}
        {surfxr::ViewerInstanceLock recovered(directory);}
        surfxr::tests::viewer_command_checks(directory);
        surfxr::tests::viewer_startup_checks(path);
        require(access((std::string(directory)+"/viewer-command.sock").c_str(),F_OK)!=0,"mode_socket_not_cleaned_up");
        {surfxr::ViewerInstanceLock recovered(directory);surfxr::ViewerModeCommands commands(directory);require(!commands.poll(),"mode_restarted_with_stale_request");}
        {
            // Two-eye counters and their timestamp must always cross threads
            // as one immutable sample, even when publication overlaps reads.
            surfxr::ViewerDecodeTelemetrySlot slot;std::atomic_bool done{false};
            require(slot.snapshot().sample_monotonic_ms==0,"decode_snapshot_invented_measurement");
            std::thread producer([&]{
                for(std::uint64_t sequence=1;sequence<=10000;++sequence){
                    surfxr::ViewerDecodeTelemetry sample;sample.sample_monotonic_ms=sequence;
                    sample.vr_decoders[0].frames=sequence;sample.vr_decoders[1].frames=sequence*2;
                    slot.publish(sample);sample.vr_decoders[0].frames=0;
                }
                done.store(true);
            });
            bool consistent=true;
            do{const auto sample=slot.snapshot();
                consistent&=sample.vr_decoders[0].frames==sample.sample_monotonic_ms&&sample.vr_decoders[1].frames==sample.sample_monotonic_ms*2;
            }while(!done.load());
            producer.join();
            const auto final=slot.snapshot();
            require(consistent&&final.sample_monotonic_ms==10000&&final.vr_decoders[0].frames==10000&&final.vr_decoders[1].frames==20000,
                    "decode_snapshot_torn_or_retained_mutable_state");
        }
        surfxr::ViewerStatusWriter disabled("",false);disabled.presented(true);disabled.audio_output({});disabled.finish(false);
        {
            surfxr::ViewerStatusWriter status(path,true);surfxr::TrackingUploadTelemetry sample;
            sample.qos_requested=sample.qos_applied=true;sample.qos_tos=184;sample.qos_priority=6;
            sample.sample_monotonic_ns=1'234'567'890'123'456;sample.requested_hz=120;
            sample.packets=120;sample.missed_deadlines=2;sample.fresh_pose_packets=117;sample.repeated_pose_packets=2;sample.invalid_pose_packets=1;
            sample.interval_samples=119;sample.interval_ns_sum=1'002'000'000;sample.interval_ns_max=20'000'000;
            sample.send_duration_ns_sum=60'000'000;sample.send_duration_ns_max=3'000'000;
            sample.encoded_bytes_sum=36000;sample.encoded_bytes_max=320;
            sample.sample_age_samples=118;sample.sample_age_ns_sum=240'000'000;sample.sample_age_ns_max=7'000'000;
            sample.window_started_ns=1'234'566'880'123'456;sample.window_ended_ns=sample.sample_monotonic_ns;sample.window_packets=120;
            sample.window_interval_ns_max=19'000'000;sample.window_send_duration_ns_max=2'000'000;sample.window_sample_age_ns_max=6'000'000;
            status.tracking_upload(sample);sample.packets=0; // Writer owns a copy.
            surfxr::ViewerTelemetry metrics;metrics.test_head_motion.enabled=true;metrics.test_head_motion.samples=240;
            metrics.haptics.positive_samples=7;metrics.haptic_apply_requests={3,4};metrics.haptic_apply_accepted={3,4};
            metrics.test_head_motion.sent_packets=238;metrics.test_head_motion.elapsed_seconds=2;
            metrics.test_head_motion.yaw_degrees=25;metrics.test_head_motion.pitch_degrees=-12;
            metrics.test_head_motion.feedback_frames=230;metrics.test_head_motion.matched_feedback_frames=229;
            metrics.test_head_motion.feedback_tracking_id=239;metrics.test_head_motion.feedback_rotation_max_degrees=27;
            metrics.test_head_motion.feedback_error_degrees=.5;
            status.telemetry(metrics); // XR updates cannot erase upload telemetry.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            bool observed=false;
            do{observed=read(path).find("\"tracking_upload\":{\"sample_monotonic_ns\":1234567890123456")!=std::string::npos;
                if(!observed)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }while(!observed&&std::chrono::steady_clock::now()<deadline);
            require(observed,"tracking_upload_live_snapshot_not_published");status.finish(false);
            const auto text=read(path);
            for(const auto* expected:{"\"test_head_motion\":{\"enabled\":true,\"samples\":240,\"sent_packets\":238",
                "\"elapsed_seconds\":2,\"yaw_degrees\":25,\"pitch_degrees\":-12",
                "\"feedback_frames\":230,\"matched_feedback_frames\":229,\"feedback_tracking_id\":239",
                "\"feedback_rotation_max_degrees\":27,\"feedback_error_degrees\":0.5"})
                require(text.find(expected)!=std::string::npos,"test_head_motion_numeric_evidence_missing");
            for(const auto* expected:{"\"requested_hz\":120","\"packets\":120","\"missed_deadlines\":2",
                "\"qos_requested\":1","\"qos_applied\":1","\"qos_tos\":184","\"qos_priority\":6",
                "\"qos_tos_errno\":0","\"qos_priority_errno\":0",
                "\"fresh_pose_packets\":117","\"repeated_pose_packets\":2","\"invalid_pose_packets\":1",
                "\"interval_samples\":119","\"interval_ns_sum\":1002000000","\"interval_ns_max\":20000000",
                "\"send_duration_ns_sum\":60000000","\"send_duration_ns_max\":3000000",
                "\"encoded_bytes_sum\":36000","\"encoded_bytes_max\":320","\"sample_age_samples\":118",
                "\"sample_age_ns_sum\":240000000","\"sample_age_ns_max\":7000000",
                "\"window_started_ns\":1234566880123456","\"window_ended_ns\":1234567890123456","\"window_packets\":120",
                "\"window_interval_ns_max\":19000000","\"window_send_duration_ns_max\":2000000","\"window_sample_age_ns_max\":6000000"})
                require(text.find(expected)!=std::string::npos,"tracking_upload_json_field_missing_or_overwritten");
            require(text.size()<=surfxr::viewer_status_max_bytes&&text.find(",}")==std::string::npos,"tracking_upload_status_not_bounded_json");
            const auto haptics=read(path+".haptics.json");
            require(haptics.size()<=2048&&haptics.find("\"apply_accepted\":[3,4]")!=std::string::npos&&
                    haptics.find("\"positive_samples\":7")!=std::string::npos,"haptics_snapshot_missing_or_not_bounded");
        }
        {
            surfxr::ViewerStatusWriter status(path,true);surfxr::ViewerAudioTelemetry sample;
            sample.requested=sample.supported=true;sample.sample_monotonic_ms=1234567890123456ULL;sample.received_pcm_bytes=9876543210ULL;
            auto& audio=sample.stats;audio.initialized=true;audio.ready=false;audio.reconnecting=true;audio.failed=true;
            audio.pulse_error=11;audio.context_state=5;audio.stream_state=3;
            audio.connection_attempts=4;audio.successful_connections=2;audio.reconnects=1;audio.connection_failures=2;audio.queue_resets=3;
            audio.submitted_bytes=1234567890;audio.pcm_frames_consumed=300001;audio.silence_frames=2201;
            audio.dropped_old_frames=21;audio.dropped_contention_frames=22;audio.underrun_callbacks=23;
            audio.server_underflows=24;audio.invalid_submissions=25;audio.submissions_after_stop=26;
            audio.write_callbacks=27;audio.written_frames=28;audio.queued_frames=29;audio.max_queued_frames=30;audio.partial_bytes=3;
            audio.server_buffer_bytes=19200;audio.server_target_bytes=7680;
            status.audio_output(sample);sample={}; // Writer owns values, not a live AudioOutput or caller reference.
            surfxr::ViewerTelemetry metrics;metrics.vr_submitted_frames=77;
            status.telemetry(metrics);status.tracking_upload({}); // Independent producers cannot erase audio.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            bool observed=false;
            do{observed=read(path).find("\"received_pcm_bytes\":9876543210")!=std::string::npos;
                if(!observed)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }while(!observed&&std::chrono::steady_clock::now()<deadline);
            require(observed,"audio_live_snapshot_not_published");
            status.finish(false);status.audio_output({}); // Late owner updates cannot replace the final sample.
            const auto text=read(path);const auto start=text.find("\"audio_output\":{");
            require(start!=std::string::npos,"audio_status_missing");const auto output=text.substr(start);
            for(const auto* expected:{"\"requested\":true","\"supported\":true","\"sample_monotonic_ms\":1234567890123456",
                "\"received_pcm_bytes\":9876543210","\"initialized\":true","\"ready\":false","\"reconnecting\":true",
                "\"failed\":true","\"stopped\":false","\"pulse_error\":11","\"context_state\":5","\"stream_state\":3",
                "\"connection_attempts\":4","\"successful_connections\":2","\"reconnects\":1","\"connection_failures\":2","\"queue_resets\":3",
                "\"submitted_bytes\":1234567890","\"pcm_frames_consumed\":300001","\"silence_frames\":2201",
                "\"dropped_old_frames\":21","\"dropped_contention_frames\":22","\"underrun_callbacks\":23",
                "\"server_underflows\":24","\"invalid_submissions\":25","\"submissions_after_stop\":26",
                "\"write_callbacks\":27","\"written_frames\":28","\"queued_frames\":29","\"max_queued_frames\":30",
                "\"partial_bytes\":3","\"server_buffer_bytes\":19200","\"server_target_bytes\":7680"})
                require(output.find(expected)!=std::string::npos,"audio_status_field_missing_or_overwritten");
            require(text.find("\"vr_submitted_frames\":77")!=std::string::npos,"audio_status_overwrote_xr_snapshot");
            require(text.find("\"streaming\"")==std::string::npos,"audio_status_acknowledged_presentation");
            require(text.size()<=surfxr::viewer_status_max_bytes&&text.find(",}")==std::string::npos,"audio_status_not_bounded_json");
        }
        {
            surfxr::FrameTimingRecorder timing;surfxr::FrameTimingRecord record;
            record.source_timestamp_us=1234;timing.record(record);
            require(!timing.enabled()&&timing.snapshot().count==0&&timing.snapshot().generation==0,"timing_recorded_when_disabled");
            timing.set_enabled(true);
            for(std::uint64_t i=1;i<=517;++i){
                record.source_timestamp_us=i;record.has_retained_first_primary_metadata=true;
                record.retained_first_primary_metadata_tracking_frame_id=static_cast<std::int32_t>(i);
                timing.record(record);
            }
            timing.set_enabled(true); // A duplicate enable must not discard evidence.
            const auto snapshot=timing.snapshot();
            require(snapshot.enabled&&snapshot.generation==1&&snapshot.count==512&&snapshot.total_recorded==517&&snapshot.overwritten==5,
                    "timing_ring_not_bounded");
            for(std::size_t i=0;i<snapshot.count;++i){
                const auto& item=snapshot.records[i];
                require(item.frame_index==i+6&&item.source_timestamp_us==i+6,"timing_ring_order_or_identity_lost");
                require(item.has_retained_first_primary_metadata&&item.retained_first_primary_metadata_tracking_frame_id==static_cast<std::int32_t>(i+6),
                        "timing_ring_mixed_retained_metadata_between_frames");
            }
            timing.set_enabled(false);record.source_timestamp_us=999;timing.record(record);
            const auto stopped=timing.snapshot();
            require(!stopped.enabled&&stopped.count==512&&stopped.total_recorded==517&&stopped.records[511].source_timestamp_us==517,
                    "timing_disable_must_retain_final_trace");
            timing.set_enabled(true);timing.record(record);
            const auto restarted=timing.snapshot();
            require(restarted.generation==2&&restarted.count==1&&restarted.total_recorded==1&&restarted.overwritten==0&&
                    restarted.records[0].frame_index==518&&restarted.records[0].source_timestamp_us==999,
                    "timing_reenable_not_distinct_or_not_cleared");
            require(snapshot.records[511].source_timestamp_us==517,"timing_snapshot_retained_mutable_ring");
        }
        {
            const auto trace_path=path+".timing.json";
            {surfxr::ViewerStatusWriter status(path,false);status.timing(std::nullopt);status.finish(false);}
            require(access(trace_path.c_str(),F_OK)!=0,"disabled_trace_created_file");
            surfxr::FrameTimingRecorder timing;timing.set_enabled(true);
            surfxr::FrameTimingRecord record;record.wait_return_ns=11;record.before_end_ns=22;record.after_end_ns=33;
            record.input_sample_ns=100;record.system_sample_ns=95;record.shortcut_reset_serial=7;
            record.shortcut_active=2;record.shortcut_down=2;record.system_available=3;record.system_clicks=1;
            record.input_focused=true;record.shortcut_guard=true;record.shortcut_toggle=true;
            record.after_input_ns=12;record.after_stage_ns=15;record.after_render_ns=20;
            record.predicted_display_time=44;record.predicted_display_period=6944444;
            record.source_timestamp_us=std::numeric_limits<std::uint64_t>::max();record.origin_epoch=7;record.pair_ready_ns=55;
            record.decoded_left_ns=66;record.decoded_right_ns=77;record.pose_query_ns=88;
            record.packet_left_ns=41;record.packet_right_ns=42;record.conversion_left_ns=51;record.conversion_right_ns=52;
            record.pose_query_xr_time=-99;record.pose_target_xr_time=111;record.result=-42;
            record.has_retained_first_primary_metadata=true;
            record.retained_first_primary_metadata_tracking_frame_id=std::numeric_limits<std::int32_t>::max();
            record.should_render=true;record.has_vr=true;record.reused=false;
            for(unsigned i=0;i<512;++i)timing.record(record);
            {
                surfxr::ViewerStatusWriter status(path,true);status.timing(timing.snapshot());
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                bool observed=false;
                do{const auto value=read(trace_path);observed=value.find("\"count\":512,\"records\":[")!=std::string::npos&&
                        value.size()>=3&&value.compare(value.size()-3,3,"]}\n")==0;
                    if(!observed)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }while(!observed&&std::chrono::steady_clock::now()<deadline);
                require(observed,"timing_live_snapshot_not_published_completely");
                timing.set_enabled(false);status.timing(timing.snapshot());status.finish(false);
            }
            const auto text=read(trace_path);
            // Numeric input diagnostics retain the same fixed 512-record ring.
            require(text.size()<1024*1024&&std::count(text.begin(),text.end(),'{')==513&&std::count(text.begin(),text.end(),'}')==513&&
                    std::count(text.begin(),text.end(),'[')==1&&std::count(text.begin(),text.end(),']')==1&&
                    text.find(",]")==std::string::npos&&text.find(",}")==std::string::npos&&text.compare(text.size()-3,3,"]}\n")==0,
                    "timing_json_not_complete_and_bounded");
            for(const auto* expected:{"\"schema_version\":1","\"enabled\":false","\"generation\":1","\"capacity\":512",
                "\"total_recorded\":512","\"overwritten\":0","\"count\":512","\"frame_index\":1,","\"frame_index\":512,",
                "\"wait_return_ns\":11","\"before_end_ns\":22","\"after_end_ns\":33","\"predicted_display_time\":44",
                "\"after_input_ns\":12","\"after_stage_ns\":15","\"after_render_ns\":20",
                "\"input_sample_ns\":100","\"system_sample_ns\":95","\"shortcut_reset_serial\":7",
                "\"shortcut_active\":2","\"shortcut_down\":2","\"system_available\":3","\"system_clicks\":1",
                "\"input_focused\":1","\"shortcut_guard\":1","\"shortcut_toggle\":1",
                "\"predicted_display_period\":6944444","\"source_timestamp_us\":18446744073709551615","\"origin_epoch\":7",
                "\"pair_ready_ns\":55","\"decoded_left_ns\":66","\"decoded_right_ns\":77","\"pose_query_ns\":88",
                "\"packet_left_ns\":41","\"packet_right_ns\":42","\"conversion_left_ns\":51","\"conversion_right_ns\":52",
                "\"pose_query_xr_time\":-99","\"pose_target_xr_time\":111","\"result\":-42",
                "\"retained_first_primary_metadata_tracking_frame_id\":2147483647",
                "\"should_render\":true,\"has_vr\":true,\"reused\":false"})
                require(text.find(expected)!=std::string::npos,"timing_json_field_missing_or_precision_lost");
            struct stat info{};require(stat(trace_path.c_str(),&info)==0&&(info.st_mode&0777)==0600,"timing_file_not_private");
            require(read(path).size()<=surfxr::viewer_status_max_bytes&&read(path).find("\"records\"")==std::string::npos,"timing_expanded_supervisor_status");
            unlink(trace_path.c_str());
        }
        {
            surfxr::FrameTimingSnapshot snapshot;snapshot.count=1;
            std::ostringstream absent;surfxr::write_frame_timing_json(absent,snapshot,1,2);
            require(absent.str().find("retained_first_primary_metadata_")==std::string::npos,
                    "timing_invented_metadata_without_selected_pair");
            auto& record=snapshot.records[0];record.has_retained_first_primary_metadata=true;
            record.retained_first_primary_metadata_tracking_frame_id=-1;
            std::ostringstream invalid;surfxr::write_frame_timing_json(invalid,snapshot,1,2);
            for(const auto* expected:{"\"retained_first_primary_metadata_tracking_frame_id\":-1"})
                require(invalid.str().find(expected)!=std::string::npos,"timing_nonfinite_metadata_not_valid_json");
        }
        {
            surfxr::ViewerStatusWriter status(path,false);
            require(read(path).find("\"starting\"")!=std::string::npos,"status_claimed_streaming_before_presentation");
            require(status.presented(true),"status_initial_presentation_not_reported");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            bool observed=false;
            do{const auto value=read(path);observed=value.find("\"streaming\"")!=std::string::npos&&value.find("\"vr\"")!=std::string::npos;
                if(!observed)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }while(!observed&&std::chrono::steady_clock::now()<deadline);
            require(observed,"status_did_not_publish_presented_mode");
            status.mode_requested(false);require(!status.presented(true),"status_old_mode_claimed_new_presentation");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(read(path+".mode.json").find("\"mode_presented_ms\":0")!=std::string::npos,"status_counted_old_mode_as_first_frame");
            require(status.presented(false),"status_changed_mode_not_reported");require(!status.presented(false),"status_repeat_presentation_reported_new_mode");
            const auto first_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(read(path+".mode.json").find("\"mode_presented_ms\":0")!=std::string::npos&&std::chrono::steady_clock::now()<first_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(read(path+".mode.json").find("\"mode_presented_ms\":0")==std::string::npos&&read(path+".mode.json").find("\"mode_request_serial\":1")!=std::string::npos,"status_first_frame_not_published");
            const auto first_mode=read(path+".mode.json");require(first_mode.size()<=256,"mode_timing_not_bounded");
            for(unsigned i=0;i<1000;++i)status.presented(i%2);
            status.finish(false);status.presented(true);status.finish(true);
            require(read(path+".mode.json")==first_mode,"status_replaced_first_frame_on_repeated_presentation");
            require(read(path).find("\"stopped\"")!=std::string::npos,"status_finish_not_final_or_idempotent");
        }
        struct stat mode{};require(stat(path.c_str(),&mode)==0&&(mode.st_mode&0777)==0600,"status_not_private");
        {surfxr::ViewerStatusWriter status(path,false);status.finish(true);}
        require(read(path).find("\"viewer_error\"")!=std::string::npos,"status_error_missing");
        {
            surfxr::ViewerStatusWriter status(path,true);surfxr::ViewerTelemetry metrics;
            metrics.sample_monotonic_ms=123456789;
            metrics.projection_width=4608;metrics.projection_height=4896;metrics.desktop_visible_mask=5;
            metrics.desktop_decoded_width={2560,1440,1920,0};metrics.desktop_decoded_height={1440,2560,1080,0};
            metrics.desktop_panel_new_frames={111,222,333,444};metrics.desktop_new_frames=111;
            metrics.desktop_layout_fingerprint=9876543210123456ULL;metrics.desktop_stage_copy_bytes=14745600000ULL;
            metrics.desktop_direct_upload_frames=1111;metrics.desktop_direct_upload_bytes=14700000000ULL;
            metrics.desktop_edge_patch_bytes=63936000;metrics.desktop_uploads=1301;metrics.desktop_cached_upload_skips=801;
            metrics.desktop_stage_copy_ms_sum=25.125;metrics.desktop_swapchain_wait_calls=1501;metrics.desktop_swapchain_wait_ms_sum=12.25;
            metrics.desktop_end_frame_ms_sum=30.5;metrics.desktop_frame_work_ms_sum=1000.75;
            metrics.desktop_gpu_timestamps=true;metrics.desktop_mip_gpu_samples=100;metrics.desktop_projection_gpu_samples=1200;
            metrics.desktop_mip_gpu_ms_sum=275.25;metrics.desktop_mip_gpu_ms_max=3.5;
            metrics.desktop_projection_gpu_ms_sum=840.5;metrics.desktop_projection_gpu_ms_max=1.25;
            metrics.decode.sample_monotonic_ms=123456000;
            auto& decoder=metrics.decode.vr_decoders[0];decoder.active=true;
            decoder.packets=1400;decoder.frames=1399;decoder.decode_time_ns=12500000000ULL;
            decoder.avcodec_send_time_ns=125000000;decoder.avcodec_receive_time_ns=300000000;
            decoder.rgba_converted_frames=1300;decoder.rgba_skipped_frames=100;decoder.rgba_suppressed_publications=2;
            decoder.rgba_conversion_time_ns=12000000000ULL;decoder.color_space=1;decoder.color_range=2;
            decoder.effective_color_space=1;decoder.effective_color_range=2;
            decoder.async_queue_packets_max=3;decoder.async_queue_bytes_max=8388608;
            decoder.async_backpressure_wait_ns=2500000;decoder.async_discarded_packets=2;
            decoder.callback_packets=1401;decoder.packet_intervals=1400;decoder.packet_interval_ns_sum=11666666666ULL;
            metrics.transition_frames=20;metrics.transition_cached_frames=12;metrics.transition_placeholder_frames=8;
            metrics.last_layer_count=2;metrics.last_cached_desktop_mask=7;metrics.last_vr_requested=true;
            metrics.last_should_render=true;metrics.last_stage_locatable=true;metrics.last_transition_pending=true;
            metrics.vr_timewarp_frames=290;metrics.vr_timewarp_fallback_frames=10;
            metrics.vr_submitted_frames=1500;metrics.vr_new_stereo_frames=1200;metrics.vr_reused_stereo_frames=300;
            metrics.vr_submit_intervals=1499;metrics.vr_submit_interval_ms_sum=12500.5;
            metrics.vr_decoded_age_samples=1200;metrics.vr_decoded_age_ms_sum=15000.25;
            metrics.vr_pose_sample_age_samples=1199;metrics.vr_pose_sample_age_ms_sum=48000.5;
            metrics.vr_submitted_pose_age_samples=1500;metrics.vr_submitted_pose_age_ms_sum=64000.75;metrics.vr_submitted_pose_age_ms_max=200.5;
            metrics.vr_foveation_frames=1200;metrics.vr_foveation_gpu_wait_ms_sum=3600.25;
            metrics.vr_foveation_gpu_samples=1199;metrics.vr_foveation_gpu_ms_sum=4796.5;
            metrics.vr_deferred_gpu_submissions=1200;metrics.vr_deferred_gpu_completions=1199;
            metrics.vr_upload_wait_calls=1199;metrics.vr_upload_wait_ms_sum=12.25;metrics.vr_upload_wait_ms_max=.5;
            metrics.vr_stage_calls=2400;metrics.vr_stage_ms_sum=2401.25;metrics.vr_stage_copy_ms_sum=1800.125;
            metrics.vr_direct_sample_frames=1200;metrics.vr_upload_avoided_bytes=21390950400ULL;
            metrics.vr_direct_upload_frames=2400;metrics.vr_direct_upload_bytes=42781900800ULL;
            metrics.vr_paired_frames=1203;metrics.vr_queued_presentations=1200;
            metrics.vr_pair_queue_overflows=1;metrics.vr_pair_queue_stale=2;metrics.vr_pair_queue_skipped=3;
            metrics.vr_swapchain_wait_calls=4800;metrics.vr_swapchain_wait_ms_sum=240.5;
            metrics.vr_end_frame_ms_sum=1800.75;metrics.vr_frame_work_ms_sum=6000.25;
            metrics.motion_sync_updates=1199;metrics.motion_sync_latency_ms=44.5;metrics.motion_sync_previous_draw_ms=7.25;
            metrics.motion_sync_gpu_estimated_frames=1198;metrics.motion_sync_gpu_unavailable_frames=1;
            metrics.motion_sync_gpu_tail_ms_sum=4792.5;metrics.motion_sync_gpu_tail_ms_max=7.125;
            metrics.prediction_time_valid_samples=1600;metrics.prediction_lead_ms_mean=43.125;
            metrics.current_refresh_hz=144;metrics.render_latency_ms=3.25;metrics.render_latency_valid=true;
            metrics.vr_eye_width=3072;metrics.vr_eye_height=3264;metrics.vr_decoded_eye_width=2048;metrics.vr_decoded_eye_height=2176;
            for(auto& desktop:metrics.decode.desktop_decoders){desktop.active=true;desktop.frames=123456;
                desktop.nv12_published_frames=123456;desktop.nv12_zero_copy_frames=123456;desktop.width=3840;desktop.height=2160;}
            status.telemetry(metrics);status.finish(false);
            const auto text=read(path);
            require(text.find("\"streaming\"")==std::string::npos,"transition_telemetry_must_not_acknowledge_mode");
            require(text.size()<=surfxr::viewer_status_max_bytes,"vr_status_exceeds_supervisor_read_limit");
            require(text.find("\"desktop_decoders\":[{\"active\":true,\"frames\":123456")!=std::string::npos,"desktop_decode_telemetry_missing");
            require(text.find("\"last_setting_request_accepted\":false")!=std::string::npos,"setting_ack_not_json_boolean");
            for(const auto* expected:{"\"sample_monotonic_ms\":123456789","\"vr_submitted_frames\":1500","\"vr_new_stereo_frames\":1200",
                "\"projection_width\":4608","\"projection_height\":4896","\"desktop_visible_mask\":5",
                "\"desktop_decoded_width\":[2560,1440,1920,0]","\"desktop_decoded_height\":[1440,2560,1080,0]",
                "\"desktop_panel_new_frames\":[111,222,333,444]","\"desktop_new_frames\":111",
                "\"desktop_layout_fingerprint\":9876543210123456","\"desktop_stage_copy_bytes\":14745600000",
                "\"desktop_direct_upload_frames\":1111","\"desktop_direct_upload_bytes\":14700000000",
                "\"desktop_edge_patch_bytes\":63936000","\"desktop_uploads\":1301","\"desktop_cached_upload_skips\":801",
                "\"desktop_stage_copy_ms_sum\":25.125","\"desktop_swapchain_wait_calls\":1501","\"desktop_swapchain_wait_ms_sum\":12.25",
                "\"desktop_end_frame_ms_sum\":30.5","\"desktop_frame_work_ms_sum\":1000.75","\"desktop_gpu_timestamps\":1",
                "\"desktop_mip_gpu_samples\":100","\"desktop_projection_gpu_samples\":1200",
                "\"desktop_mip_gpu_ms_sum\":275.25","\"desktop_mip_gpu_ms_max\":3.5",
                "\"desktop_projection_gpu_ms_sum\":840.5","\"desktop_projection_gpu_ms_max\":1.25",
                "\"decoder_sample_monotonic_ms\":123456000","\"vr_decoders\":[{\"active\":true,\"packets\":1400,\"frames\":1399",
                "\"decode_time_ns\":12500000000","\"avcodec_send_time_ns\":125000000","\"avcodec_receive_time_ns\":300000000",
                "\"rgba_conversion_time_ns\":12000000000","\"rgba_converted_frames\":1300","\"rgba_skipped_frames\":100","\"rgba_suppressed_publications\":2","\"callback_packets\":1401","\"packet_intervals\":1400","\"packet_interval_ns_sum\":11666666666",
                "\"async_queue_packets_max\":3","\"async_queue_bytes_max\":8388608","\"async_backpressure_wait_ns\":2500000","\"async_discarded_packets\":2",
                "\"color_space\":1,\"color_range\":2,\"effective_color_space\":1,\"effective_color_range\":2","{\"active\":false,\"packets\":0,\"frames\":0",
                "\"vr_timewarp_frames\":290","\"vr_timewarp_fallback_frames\":10",
                "\"vr_reused_stereo_frames\":300","\"vr_submit_intervals\":1499","\"vr_submit_interval_ms_sum\":12500.5",
                "\"vr_decoded_age_samples\":1200","\"vr_decoded_age_ms_sum\":15000.25","\"vr_pose_sample_age_samples\":1199",
                "\"vr_submitted_pose_age_samples\":1500","\"vr_submitted_pose_age_ms_sum\":64000.75","\"vr_submitted_pose_age_ms_max\":200.5",
                "\"vr_pose_sample_age_ms_sum\":48000.5","\"vr_foveation_frames\":1200","\"vr_foveation_gpu_wait_ms_sum\":3600.25",
                "\"vr_foveation_gpu_samples\":1199","\"vr_foveation_gpu_ms_sum\":4796.5","\"vr_deferred_gpu_submissions\":1200",
                "\"vr_deferred_gpu_completions\":1199","\"vr_stage_calls\":2400","\"vr_stage_ms_sum\":2401.25","\"vr_stage_copy_ms_sum\":1800.125",
                "\"vr_direct_sample_frames\":1200","\"vr_upload_avoided_bytes\":21390950400",
                "\"vr_direct_upload_frames\":2400","\"vr_direct_upload_bytes\":42781900800",
                "\"vr_paired_frames\":1203","\"vr_queued_presentations\":1200","\"vr_pair_queue_overflows\":1","\"vr_pair_queue_stale\":2","\"vr_pair_queue_skipped\":3",
                "\"vr_upload_wait_calls\":1199","\"vr_upload_wait_ms_sum\":12.25","\"vr_upload_wait_ms_max\":0.5",
                "\"vr_swapchain_wait_calls\":4800","\"vr_swapchain_wait_ms_sum\":240.5","\"vr_end_frame_ms_sum\":1800.75","\"vr_frame_work_ms_sum\":6000.25",
                "\"motion_sync_updates\":1199","\"motion_sync_latency_ms\":44.5","\"motion_sync_previous_draw_ms\":7.25",
                "\"motion_sync_gpu_estimated_frames\":1198","\"motion_sync_gpu_unavailable_frames\":1",
                "\"motion_sync_gpu_tail_ms_sum\":4792.5","\"motion_sync_gpu_tail_ms_max\":7.125",
                "\"prediction_time_valid_samples\":1600","\"prediction_lead_ms_mean\":43.125","\"current_refresh_hz\":144",
                "\"render_latency_ms\":3.25","\"render_latency_valid\":1","\"vr_eye_width\":3072","\"vr_eye_height\":3264",
                "\"vr_decoded_eye_width\":2048","\"vr_decoded_eye_height\":2176",
                "\"transition_frames\":20","\"transition_cached_frames\":12","\"transition_placeholder_frames\":8",
                "\"last_cached_desktop_mask\":7","\"last_layer_count\":2","\"last_transition_pending\":1"})
                require(text.find(expected)!=std::string::npos,"vr_status_numeric_metric_missing");
        }
        {
            // Long-running counters plus image settings must not make the
            // launcher discard a healthy viewer's status/setting acknowledgments.
            surfxr::ViewerStatusWriter status(path,true);surfxr::ViewerTelemetry metrics;
            constexpr std::uint64_t large=1234567890123456789ULL;
            metrics.decode.sample_monotonic_ms=large;
            for(auto& decoder:metrics.decode.vr_decoders){
                decoder.packets=decoder.frames=decoder.configuration_changes=decoder.decode_time_ns=large;
                decoder.avcodec_send_time_ns=decoder.avcodec_receive_time_ns=decoder.rgba_conversion_time_ns=large;
                decoder.rgba_converted_frames=decoder.rgba_skipped_frames=decoder.rgba_suppressed_publications=large;
                decoder.rgba_buffer_allocations=decoder.rgba_buffer_reuses=decoder.rgba_temporary_allocations=decoder.rgba_pool_bytes=large;
                decoder.metadata_unmatched=decoder.sequence_restarts=decoder.callback_packets=decoder.packet_intervals=decoder.packet_interval_ns_sum=large;
                decoder.async_queue_packets_max=decoder.async_queue_bytes_max=decoder.async_backpressure_wait_ns=decoder.async_discarded_packets=large;
                decoder.width=decoder.height=8192;decoder.color_space=10;decoder.color_range=2;decoder.active=true;
                decoder.effective_color_space=10;decoder.effective_color_range=2;
            }
            metrics.sample_monotonic_ms=metrics.desktop_frames=metrics.desktop_intervals=metrics.stage_calls=metrics.projection_calls=large;
            metrics.desktop_panel_new_frames.fill(large);metrics.desktop_decoded_width.fill(8192);metrics.desktop_decoded_height.fill(8192);
            metrics.desktop_layout_fingerprint=metrics.desktop_stage_copy_bytes=metrics.desktop_swapchain_wait_calls=large;
            metrics.desktop_direct_upload_frames=metrics.desktop_direct_upload_bytes=metrics.desktop_edge_patch_bytes=large;
            metrics.desktop_uploads=metrics.desktop_cached_upload_skips=large;
            metrics.desktop_mip_gpu_samples=metrics.desktop_projection_gpu_samples=large;
            metrics.projection_width=metrics.projection_height=8192;metrics.desktop_visible_mask=15;metrics.desktop_gpu_timestamps=true;
            metrics.desktop_stage_copy_ms_sum=metrics.desktop_swapchain_wait_ms_sum=metrics.desktop_end_frame_ms_sum=metrics.desktop_frame_work_ms_sum=1234567890123.125;
            metrics.desktop_mip_gpu_ms_sum=metrics.desktop_mip_gpu_ms_max=metrics.desktop_projection_gpu_ms_sum=metrics.desktop_projection_gpu_ms_max=1234567890123.125;
            metrics.desktop_new_frames=metrics.desktop_new_intervals=metrics.desktop_quality_frames=metrics.desktop_mip_updates=metrics.desktop_mip_reuses=large;
            metrics.vr_timewarp_frames=metrics.vr_timewarp_fallback_frames=large;
            metrics.vr_submitted_frames=metrics.vr_new_stereo_frames=metrics.vr_reused_stereo_frames=metrics.vr_submit_intervals=large;
            metrics.vr_decoded_age_samples=metrics.vr_submitted_pose_age_samples=metrics.vr_pose_sample_age_samples=metrics.vr_foveation_frames=metrics.motion_sync_updates=large;
            metrics.prediction_time_valid_samples=metrics.vr_enhancement_frames=metrics.vr_upscaled_frames=large;
            metrics.vr_enhancement_gpu_samples=metrics.vr_raw_enhancement_frames=large;
            metrics.vr_fused_postprocess_frames=large;
            metrics.vr_deferred_gpu_submissions=metrics.vr_deferred_gpu_completions=metrics.vr_foveation_gpu_samples=large;
            metrics.vr_upload_wait_calls=metrics.motion_sync_gpu_estimated_frames=metrics.motion_sync_gpu_unavailable_frames=large;
            metrics.vr_upload_wait_ms_sum=metrics.vr_upload_wait_ms_max=metrics.motion_sync_gpu_tail_ms_sum=metrics.motion_sync_gpu_tail_ms_max=1234567890123.125;
            metrics.vr_stage_calls=metrics.vr_swapchain_wait_calls=large;
            metrics.vr_direct_upload_frames=metrics.vr_direct_upload_bytes=large;
            metrics.vr_direct_sample_frames=metrics.vr_upload_avoided_bytes=large;
            metrics.vr_paired_frames=metrics.vr_queued_presentations=metrics.vr_pair_queue_overflows=metrics.vr_pair_queue_stale=metrics.vr_pair_queue_skipped=large;
            metrics.vr_foveation_gpu_ms_sum=metrics.vr_foveation_gpu_ms_max=1234567890123.125;
            metrics.vr_stage_ms_sum=metrics.vr_stage_ms_max=metrics.vr_stage_copy_ms_sum=metrics.vr_stage_copy_ms_max=1234567890123.125;
            metrics.vr_swapchain_wait_ms_sum=metrics.vr_swapchain_wait_ms_max=metrics.vr_end_frame_ms_sum=metrics.vr_end_frame_ms_max=1234567890123.125;
            metrics.vr_frame_work_ms_sum=metrics.vr_frame_work_ms_max=1234567890123.125;
            metrics.transition_frames=metrics.transition_cached_frames=metrics.transition_placeholder_frames=large;
            metrics.vr_keyframe_recoveries=metrics.vr_restart_recoveries=metrics.mode_source_mismatches=metrics.desktop_source_retries=large;
            metrics.last_quality_request_serial=metrics.last_setting_request_serial=large;
            metrics.desktop_interval_ms=metrics.stage_ms=metrics.projection_ms=metrics.desktop_new_interval_ms=1234567890123.125;
            metrics.vr_submitted_pose_age_ms_sum=metrics.vr_submitted_pose_age_ms_max=1234567890123.125;
            metrics.vr_submit_interval_ms_sum=metrics.vr_decoded_age_ms_sum=metrics.vr_pose_sample_age_ms_sum=1234567890123.125;
            metrics.vr_foveation_gpu_wait_ms_sum=metrics.vr_enhancement_gpu_wait_ms_sum=1234567890123.125;
            metrics.vr_enhancement_gpu_ms_sum=1234567890123.125;
            metrics.vr_timewarp=false;
            metrics.last_quality_request_accepted=true;metrics.vr_upscaling=true;metrics.vr_sharpening_enabled=true;
            metrics.vr_controllers_enabled=false;metrics.last_input_request_serial=large;metrics.last_input_request_accepted=true;
            metrics.performance_overlay=true;metrics.performance_overlay_frames=123;metrics.performance_overlay_updates=4;
            metrics.hmd_battery_percent=57;metrics.hmd_battery_charging=true;metrics.left_battery_percent=84;metrics.right_battery_percent=26;
            metrics.hmd_raw_battery_percent=52;metrics.sync_raw_battery=true;
            metrics.mini_hud_enabled=true;metrics.mini_hud_fps=false;metrics.mini_hud_latency=true;
            metrics.mini_hud_frame_time=true;metrics.mini_hud_decode=true;metrics.mini_hud_gaze_only=true;metrics.mini_hud_gaze_marker=false;
            metrics.mini_hud_x=23;metrics.mini_hud_y=67;metrics.mini_hud_scale=135;
            surfxr::TrackingUploadTelemetry tracking;
            tracking.sample_monotonic_ns=tracking.packets=tracking.missed_deadlines=tracking.fresh_pose_packets=large;
            tracking.repeated_pose_packets=tracking.invalid_pose_packets=tracking.interval_samples=large;
            tracking.interval_ns_sum=tracking.interval_ns_max=tracking.send_duration_ns_sum=tracking.send_duration_ns_max=large;
            tracking.encoded_bytes_sum=tracking.encoded_bytes_max=tracking.sample_age_samples=tracking.sample_age_ns_sum=tracking.sample_age_ns_max=large;
            tracking.window_started_ns=tracking.window_ended_ns=tracking.window_packets=tracking.window_interval_ns_max=large;
            tracking.window_send_duration_ns_max=tracking.window_sample_age_ns_max=large;tracking.requested_hz=120;
            surfxr::ViewerAudioTelemetry audio;audio.requested=audio.supported=true;
            constexpr auto max_counter=std::numeric_limits<std::uint64_t>::max();
            audio.sample_monotonic_ms=audio.received_pcm_bytes=max_counter;
            audio.stats.connection_attempts=audio.stats.successful_connections=audio.stats.reconnects=max_counter;
            audio.stats.connection_failures=audio.stats.queue_resets=max_counter;
            audio.stats.submitted_bytes=audio.stats.pcm_frames_consumed=audio.stats.silence_frames=max_counter;
            audio.stats.dropped_old_frames=audio.stats.dropped_contention_frames=audio.stats.underrun_callbacks=max_counter;
            audio.stats.server_underflows=audio.stats.invalid_submissions=audio.stats.submissions_after_stop=max_counter;
            audio.stats.write_callbacks=audio.stats.written_frames=max_counter;
            audio.stats.queued_frames=audio.stats.max_queued_frames=audio.stats.partial_bytes=std::numeric_limits<std::uint32_t>::max();
            audio.stats.server_buffer_bytes=audio.stats.server_target_bytes=std::numeric_limits<std::uint32_t>::max();
            audio.stats.pulse_error=audio.stats.context_state=audio.stats.stream_state=std::numeric_limits<int>::min();
            metrics.surfxr_timing_available=true; // Optional HUD metrics cannot hide lifecycle acknowledgments.
            metrics.decode.desktop_decoders=metrics.decode.vr_decoders;
            status.audio_output(audio);status.tracking_upload(tracking);status.telemetry(metrics);
            for(const auto phase:{surfxr::SteamVrStartupState::Launching,surfxr::SteamVrStartupState::RuntimeReady,surfxr::SteamVrStartupState::Completed,surfxr::SteamVrStartupState::Failed}){
                status.mode_requested(true);status.steamvr_startup({phase==surfxr::SteamVrStartupState::Completed?surfxr::SteamVrStartupState::RuntimeReady:phase,
                    phase==surfxr::SteamVrStartupState::Failed?surfxr::SteamVrStartupReason::TransportFailed:surfxr::SteamVrStartupReason::None,max_counter});
                if(phase==surfxr::SteamVrStartupState::Completed)status.presented(true);
                const auto expected=std::string("\"steamvr_startup\":{\"state\":\"")+surfxr::steamvr_startup_name(phase)+"\"";
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(read(path).find(expected)==std::string::npos&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const auto snapshot=read(path);require(snapshot.find(expected)!=std::string::npos&&snapshot.size()<=surfxr::viewer_status_max_bytes&&
                    snapshot.find("\"last_quality_request_accepted\":true")!=std::string::npos&&snapshot.find("\"received_pcm_bytes\":18446744073709551615")!=std::string::npos,"large_counters_erased_startup_or_critical_fields");
            }
            status.finish(false);const auto text=read(path);
            require(text.size()>8192&&text.size()<=surfxr::viewer_status_max_bytes,"long_running_status_exceeds_bounded_reader");
            require(text.find("\"tracking_upload\":{\"sample_monotonic_ns\":1234567890123456789")!=std::string::npos,"large_tracking_counters_missing");
            require(text.find("\"received_pcm_bytes\":18446744073709551615")!=std::string::npos&&
                    text.find("\"queue_resets\":18446744073709551615")!=std::string::npos&&
                    text.find("\"context_state\":-2147483648")!=std::string::npos,"large_audio_counters_missing");
            require(text.find("\"last_quality_request_accepted\":true")!=std::string::npos,"quality_ack_not_json_boolean");
            require(text.find("\"last_input_request_accepted\":true")!=std::string::npos&&text.find("\"vr_controllers_enabled\":false")!=std::string::npos,"controller_preference_ack_not_json_boolean");
            require(text.find("\"performance_overlay\":true")!=std::string::npos&&text.find("\"performance_overlay_frames\":123")!=std::string::npos&&text.find("\"performance_overlay_updates\":4")!=std::string::npos,"hud_state_not_serialized");
            require(text.find("\"hmd_battery_percent\":57")!=std::string::npos&&text.find("\"hmd_battery_charging\":true")!=std::string::npos,"battery_telemetry_missing");
            require(text.find("\"hmd_raw_battery_percent\":52")!=std::string::npos&&text.find("\"sync_raw_battery\":true")!=std::string::npos,"raw_battery_telemetry_missing");
            for(const auto* field:{"\"mini_hud_enabled\":true","\"mini_hud_fps\":false","\"mini_hud_latency\":true",
                "\"mini_hud_frame_time\":true","\"mini_hud_decode\":true","\"mini_hud_gaze_only\":true","\"mini_hud_gaze_marker\":false",
                "\"mini_hud_x\":23","\"mini_hud_y\":67","\"mini_hud_scale\":135"})
                require(text.find(field)!=std::string::npos,"mini_hud_telemetry_value_or_type_missing");
        }
        bool rejected=false;try{surfxr::ViewerStatusWriter bad(std::string(directory)+"/missing/state",false);}catch(...){rejected=true;}
        require(rejected,"status_unwritable_path_not_rejected");
        std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":18,\"network_used\":false,\"openxr_session_started\":false}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}
    unlink((path+".mode.json").c_str());unlink((path+".haptics.json").c_str());unlink((path+".timing.json").c_str());unlink(path.c_str());unlink((std::string(directory)+"/viewer.lock").c_str());rmdir(directory);return result;
}
