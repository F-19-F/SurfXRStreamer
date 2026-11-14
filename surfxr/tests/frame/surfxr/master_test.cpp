#include "wire.hpp"
#include "stream.hpp"
#include "master_fixture.hpp"
#include <cmath>

using namespace surfxr::transport;
namespace {
Bytes hex(const std::string& text){
    Bytes result;for(std::size_t i=0;i<text.size();i+=2)result.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i,2),nullptr,16)));return result;
}
template<class F> void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"test_master_expected_rejection");}
Bytes split_config(const std::string& ext,bool tcp=true,unsigned width=32,unsigned height=32){
    Writer w(true);w.string("{\"server_version\":\"21.0.0-dev14\",\"session_settings\":{\"connection\":{\"stream_protocol\":{\"variant\":\""+std::string(tcp?"Tcp":"Udp")+"\"}}}}");
    w.integer(width,4);w.integer(height,4);w.real(120);w.integer(48000,4);w.boolean(false);w.real(1);w.boolean(false);w.boolean(false);w.string(ext);return w.bytes;
}
}
void master_checks(){
    require(!stream_config(split_config("{}"),"21.0.0-dev14").pose_time,"pose time defaults to legacy");
    require(stream_config(split_config("{\"frame_pose_time\":1}"),"21.0.0-dev14").pose_time,"pose time explicit acknowledgement");
    require(stream_config(split_config("{\"frame_pose_time\":1,\"frame_clock_calibration\":1}"),"21.0.0-dev14").clock_calibration,"measured clock explicitly acknowledged");
    rejects([&]{stream_config(split_config("{\"frame_clock_calibration\":1}"),"21.0.0-dev14");});
    rejects([&]{stream_config(split_config("{\"frame_pose_time\":1,\"frame_clock_calibration\":3}"),"21.0.0-dev14");});
    for(const auto* ext:{"{}","{\"frame_pose_time\":1}","{\"frame_pose_time\":1,\"frame_display_time\":1}","{\"frame_pose_time\":1,\"frame_clock_calibration\":1}"})
        rejects([&]{validate_pose_clock(stream_config(split_config(ext),"21.0.0-dev14"),true);});
    validate_pose_clock(stream_config(split_config("{\"frame_pose_time\":1,\"frame_clock_calibration\":2}"),"21.0.0-dev14"),true);
    rejects([&]{stream_config(split_config("{\"frame_pose_time\":2}"),"21.0.0-dev14");});
    const auto timed_caps=capabilities("127.0.0.1",32,32,{120},"21.0.0-dev14",true,true,false,true,"frame",false,false,true);
    require(std::string(timed_caps.begin(),timed_caps.end()).find("\"frame_pose_time\":1")!=std::string::npos,"pose time explicit request");
    require(std::string(timed_caps.begin(),timed_caps.end()).find("\"frame_clock_calibration\":2")!=std::string::npos,"clock calibration explicit request");
    require(std::string(timed_caps.begin(),timed_caps.end()).find("frame_display_time")==std::string::npos,"no dynamic display latency contract");
    const auto frame_caps=capabilities("127.0.0.1",32,32,{90},"21.0.0-dev14",true,true,false,true);
    require(std::string(frame_caps.begin(),frame_caps.end()).find("\"frame_controller_inputs\":1")!=std::string::npos,"native Frame capability explicit");
    require(std::string(frame_caps.begin(),frame_caps.end()).find("\"frame_boundary_sync\":1")!=std::string::npos,"boundary capability explicit");
    require(stream_config(split_config("{\"frame_boundary_sync\":1}",false),"21.0.0-dev14").boundary_sync,"boundary acknowledged independently of controller layout");
    require(!stream_config(split_config("{}",false),"21.0.0-dev14").boundary_sync,"legacy keeps original playspace protocol");
    rejects([&]{stream_config(split_config("{\"frame_boundary_sync\":2}",false),"21.0.0-dev14");});
    require(stream_config(split_config("{\"frame_controller_inputs\":1}",false),"21.0.0-dev14").frame_inputs,"native Frame acknowledgement");
    require(!stream_config(split_config("{}",false),"21.0.0-dev14").frame_inputs,"legacy stream keeps Touch mapping");
    rejects([&]{stream_config(split_config("{\"frame_controller_inputs\":2}",false),"21.0.0-dev14");});
    const auto gaze_caps=capabilities("127.0.0.1",3072,3264,{120},"21.0.0-dev14",true,true,true);
    require(std::string(gaze_caps.begin(),gaze_caps.end()).find("\"frame_gaze_streaming\":1")!=std::string::npos,"test_gaze_capability");
    require(std::string(gaze_caps.begin(),gaze_caps.end()).find("\"frame_video_timestamps\":1")!=std::string::npos,"test_capture_clock_capability");
    const std::string gaze_ack="{\"frame_gaze_streaming\":{\"version\":1,\"base_width\":32,\"base_height\":32,\"focus_width\":32,\"focus_height\":32}}";
    require(bool(stream_config(split_config(gaze_ack,false),"21.0.0-dev14").gaze_atlas),"test_gaze_negotiation");
    const auto resized=stream_config(split_config("{\"frame_gaze_streaming\":{\"version\":1,\"base_width\":1024,\"base_height\":1088,\"focus_width\":640,\"focus_height\":992}}",false,3072,3264),"21.0.0-dev14");
    require(resized.gaze_atlas.focus_width==640&&resized.gaze_atlas.focus_height==992&&
        resized.gaze_atlas.base_width==1024&&resized.gaze_atlas.base_height==1088,"test_configurable_gaze_region");
    rejects([&]{stream_config(split_config(gaze_ack),"21.0.0-dev14");});
    rejects([&]{auto invalid=gaze_ack;invalid.replace(invalid.find("\"version\":1"),11,"\"version\":2");stream_config(split_config(invalid,false),"21.0.0-dev14");});
    surfxr::GazeSample gaze;gaze.sampled_monotonic_ns=1000;gaze.origin_epoch=1;gaze.valid={true,false};gaze.positions[0]={.25f,-.5f};
    const auto feedback=gaze_feedback(&gaze,1001,1);
    Bytes expected{1,251,232,3,1,1,0,0,128,62,0,0,0,191,0,0,0,0,0,0,0,0,0};
    require(feedback==expected,"test_gaze_rust_reference");
    for(auto now:std::array<std::uint64_t,2>{999,100'001'000})require(gaze_feedback(&gaze,now,1)==Bytes{0},"test_gaze_clock_bounds");
    require(gaze_feedback(&gaze,1001,2)==Bytes{0},"test_gaze_epoch_bounds");
    require(capabilities("127.0.0.1",3072,3264,{144},"21.0.0-dev14")==hex(master_fixture::capabilities),"test_master_rust_capabilities");
    const auto c=stream_config(hex(master_fixture::config),"21.0.0-dev14");
    require(c.priority_port==0,"test_master_legacy_transport");
    const std::string ack="{\"frame_split_streams\":{\"version\":1,\"priority_port\":19945}}";
    require(stream_config(split_config(ack),"21.0.0-dev14").priority_port==19945,"test_split_negotiated_port");
    rejects([&]{stream_config(split_config(ack,false),"21.0.0-dev14");});
    const std::string udp_ack="{\"frame_split_streams_udp\":{\"version\":1,\"priority_port\":19945}}";
    const auto fec_ack=udp_ack.substr(0,udp_ack.size()-1)+",\"frame_video_fec\":{\"version\":1,\"data_shards\":16}}";
    require(stream_config(split_config(fec_ack,false),"21.0.0-dev14").video_fec,"test_fec_negotiation");
    rejects([&]{stream_config(split_config("{\"frame_video_fec\":{\"version\":1,\"data_shards\":16}}",false),"21.0.0-dev14");});
    require(stream_config(split_config(udp_ack,false),"21.0.0-dev14").priority_port==19945,"test_split_udp_negotiated_port");
    rejects([&]{stream_config(split_config(ack),"21.0.0-dev14",false);});
    require(!stream_config(split_config(udp_ack,false),"21.0.0-dev14",false).tcp,"test_production_udp_only");
    const auto only_udp=capabilities("127.0.0.1",32,32,{120},"21.0.0-dev14",true,true);
    const std::string advertised(only_udp.begin(),only_udp.end());
    require(advertised.find("frame_udp_only")!=std::string::npos&&advertised.find("frame_split_streams\"")==std::string::npos,"test_no_deprecated_tcp_capability");
    require(!c.video_haptics&&stream_config(split_config("{\"frame_video_haptics\":1}"),"21.0.0-dev14").video_haptics,"test_video_haptics_requires_ack");
    require(!c.image_timestamps&&stream_config(split_config("{\"frame_video_timestamps\":1}"),"21.0.0-dev14").image_timestamps,"test_capture_clock_requires_ack");
    rejects([&]{stream_config(split_config("{\"frame_video_timestamps\":2}"),"21.0.0-dev14");});
    rejects([&]{stream_config(split_config("{\"frame_video_haptics\":2}"),"21.0.0-dev14");});
    rejects([&]{stream_config(split_config(udp_ack),"21.0.0-dev14");});
    rejects([&]{stream_config(split_config("{\"frame_split_streams\":{\"version\":1,\"priority_port\":19945},\"frame_split_streams_udp\":{\"version\":1,\"priority_port\":19945}}"),"21.0.0-dev14");});
    for(const auto* ext:{"{\"frame_split_streams\":1}","{\"frame_split_streams\":{\"version\":2,\"priority_port\":19945}}",
            "{\"frame_split_streams\":{\"version\":1,\"priority_port\":19944}}","{\"frame_split_streams\":{\"version\":1}}"})
        rejects([&]{stream_config(split_config(ext),"21.0.0-dev14");});
    require(capabilities("127.0.0.1",3072,3264,{144},"21.0.0-dev14",true)!=hex(master_fixture::capabilities),"test_master_split_capability_opt_in");
    require(c.width==3072&&c.height==3264&&c.refresh_rate==144,"test_master_rust_stream_config");
    rejects([]{stream_config(hex(master_fixture::config));});
    require(server_control(hex(master_fixture::decoder),true).config==Bytes({0,0,0,1,0x40}),"test_master_rust_decoder_config");
    surfxr::TrackingFrame frame;frame.head.flags=surfxr::OrientationValid|surfxr::PositionValid;frame.head.pose.position={1,2,3};frame.ipd_meters=.064f;
    frame.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f}}};
    require(tracking(frame,1'000'000'234,false,true)==hex(master_fixture::tracking),"test_master_rust_tracking");
    require(views(frame,true)==hex(master_fixture::views),"test_master_rust_local_views");
    const auto video=hex(master_fixture::video);Reader r(video,true);const auto header=video_header(r,true);r.end();
    require(header.image_timestamp==header.timestamp,"test_legacy_image_identity");
    auto captured=video;const auto image_stamp=1'006'944'678ull;
    for(unsigned i=0;i<8;++i)captured.push_back(std::uint8_t(image_stamp>>(8*i)));
    captured.insert(captured.end(),{0,0,0,1,0x40});
    Reader capture(captured,true);const auto identity=video_header(capture,true,false,false,true);
    require(identity.timestamp==header.timestamp&&identity.image_timestamp==image_stamp&&capture.remaining()==5,
        "test_capture_identity_keeps_original_tracking_and_nals");
    auto short_capture=captured;short_capture.resize(video.size()+7);
    rejects([&]{Reader input(short_capture,true);video_header(input,true,false,false,true);});
    require(header.timestamp==1'000'000'234&&header.idr&&header.has_views&&std::abs(header.poses[0].position.x-.968f)<.0001f,"test_master_rust_video_pose");
    require(std::abs(header.fov[0].left-std::tan(.7f))<.0001f,"test_master_rust_video_fov");
    auto extended=video;const Bytes amplitudes{0,0,128,62,0,0,0,63};extended.insert(extended.end(),amplitudes.begin(),amplitudes.end());
    const Bytes nal{0,0,0,1,0x40};extended.insert(extended.end(),nal.begin(),nal.end());
    Reader source(extended,true);const auto metadata=video_header(source,true,true);
    require(metadata.haptics==std::array<float,2>{.25f,.5f}&&source.remaining()==nal.size()&&std::equal(nal.begin(),nal.end(),source.data()),"test_video_source_haptics_leave_nals_intact");
    auto atlas_header=video;atlas_header.insert(atlas_header.end(),amplitudes.begin(),amplitudes.end());
    Writer crop;for(auto value:{100u,200u,300u,400u})crop.raw_integer(value,4);
    atlas_header.insert(atlas_header.end(),crop.bytes.begin(),crop.bytes.end());atlas_header.insert(atlas_header.end(),nal.begin(),nal.end());
    Reader atlas(atlas_header,true);const auto with_crop=video_header(atlas,true,true,true);
    require(with_crop.crops==std::array<std::array<unsigned,2>,2>{{{100,200},{300,400}}}&&atlas.remaining()==nal.size(),"test_gaze_crops_leave_nals_intact");
    rejects([&]{auto short_header=video;short_header.insert(short_header.end(),amplitudes.begin(),amplitudes.end()-1);Reader input(short_header,true);video_header(input,true,true);});
    rejects([&]{auto negative=video;Writer invalid;invalid.real(-.5f);invalid.real(0);negative.insert(negative.end(),invalid.bytes.begin(),invalid.bytes.end());Reader input(negative,true);video_header(input,true,true);});
    auto truncated=video;truncated.pop_back();rejects([&]{Reader input(truncated,true);video_header(input,true);});
    rejects([]{const Bytes invalid{255};Reader reader(invalid,true);reader.integer(8);});
    rejects([]{const Bytes oversized{253,0,0,0,0,1,0,0,0};Reader reader(oversized,true);reader.integer(4);});
    Writer values(true);for(auto value:{std::uint64_t{250},std::uint64_t{251},std::uint64_t{65535},std::uint64_t{65536},UINT64_MAX})values.integer(value,8);
    Reader reader(values.bytes,true);for(auto value:{std::uint64_t{250},std::uint64_t{251},std::uint64_t{65535},std::uint64_t{65536},UINT64_MAX})require(reader.integer(8)==value,"test_master_varint_boundaries");reader.end();
    Bytes payload(8000,0x23);const auto parts=shards(3,0,payload,1400,true);Reassembler assembler(1400,true);
    {
        Reassembler ordered(1400,true,8*1024*1024,1u<<3,2'000'000);
        const auto later=shards(3,1,payload,1400,true);
        require(!ordered.accept(parts.front(),1),"test_ordered_partial_first");
        require(!ordered.next_ready_ns(1),"test_partial_packet_needs_no_timer");
        for(const auto& part:later)require(!ordered.accept(part,2),"test_reordering_does_not_break_reference_chain");
        require(ordered.next_ready_ns(2)==2'000'002,"test_complete_gap_reports_actual_deadline");
        std::optional<Packet> first;
        for(std::size_t i=1;i<parts.size();++i)if(auto packet=ordered.accept(parts[i],3))first=std::move(packet);
        require(first&&first->index==0&&!first->loss&&first->bytes==payload,"test_reordered_first_is_restored");
        auto next=ordered.pop_ready(3);
        require(next&&next->index==1&&!next->loss&&ordered.reordered()==1&&ordered.retained_bytes()==0,"test_reordered_next_follows_reference");
        require(!ordered.next_ready_ns(3),"test_drained_reorder_deadline_cleared");
        for(const auto& part:shards(3,3,payload,1400,true))require(!ordered.accept(part,4),"test_real_loss_has_bounded_grace");
        require(!ordered.pop_ready(2'000'003),"test_no_early_gap_expiry");
        next=ordered.pop_ready(2'000'004);
        require(next&&next->index==3&&next->loss,"test_missing_packet_expires_without_new_datagrams");
        require(!ordered.accept(parts.front(),2'000'005)&&ordered.late_shards()==1,
            "test_older_shard_is_counted_without_publication");
        require(!ordered.accept(shards(3,3,payload,1400,true).front(),2'000'006)&&
            ordered.late_shards()==1&&ordered.retained_bytes()==0,
            "test_delivered_duplicate_does_not_count_as_late_or_allocate");
    }
    require(parts.front().size()==1400&&little(parts.front().data(),2)==3,"test_master_udp_prefix");
    for(std::size_t i=parts.size();i-->0;){auto packet=assembler.accept(parts[i],1);if(!i)require(packet&&packet->bytes==payload&&!packet->loss,"test_master_udp_reassembly");}
    Writer tcp;tcp.integer(3,2);tcp.integer(0,4);tcp.integer(payload.size(),4);tcp.bytes.insert(tcp.bytes.end(),payload.begin(),payload.end());
    for(std::size_t split=0;split<=tcp.bytes.size();split+=37){TcpPacketFramer framer;unsigned count=0;
        const auto sink=[&](Packet p){require(p.stream==3&&p.bytes==payload&&!p.loss,"test_master_tcp_body");++count;};
        framer.feed(tcp.bytes.data(),split,sink);framer.feed(tcp.bytes.data()+split,tcp.bytes.size()-split,sink);require(count==1,"test_master_tcp_fragmentation");
    }
    rejects([]{TcpPacketFramer framer;Writer prefix;prefix.integer(3,2);prefix.integer(0,4);prefix.integer(8*1024*1024+1,4);framer.feed(prefix.bytes.data(),prefix.bytes.size(),[](Packet){});});
    rejects([]{TcpPacketFramer framer(192000,(1u<<1)|(1u<<2));Writer prefix;prefix.integer(3,2);prefix.integer(0,4);prefix.integer(1,4);framer.feed(prefix.bytes.data(),prefix.bytes.size(),[](Packet){});});
    rejects([]{TcpPacketFramer framer(192000,(1u<<1)|(1u<<2));Writer prefix;prefix.integer(2,2);prefix.integer(0,4);prefix.integer(192001,4);framer.feed(prefix.bytes.data(),prefix.bytes.size(),[](Packet){});});
    Reassembler priority(1400,true,192000,(1u<<1)|(1u<<2));
    rejects([&]{priority.accept(parts.front(),1);});require(priority.retained_bytes()==0,"test_udp_wrong_route_no_allocation");
    auto audio=shards(2,0,Bytes(192001,1),1400,true);
    rejects([&]{priority.accept(audio.back(),1);});require(priority.retained_bytes()==0,"test_udp_oversized_no_allocation");
    Writer invalid;invalid.integer(2,2);invalid.integer(0,4);invalid.integer(UINT32_MAX,4);invalid.integer(0,4);invalid.bytes.resize(1400);
    rejects([&]{priority.accept(invalid.bytes,1);});require(priority.retained_bytes()==0,"test_udp_shard_count_no_allocation");
    audio=shards(2,0,Bytes(8000,1),1400,true);
    require(!priority.accept(audio.front(),1),"test_udp_incomplete_audio");
    const auto later=shards(2,1,Bytes(8000,2),1400,true);
    for(std::size_t i=later.size();i-->0;){auto packet=priority.accept(later[i],2);if(!i)require(packet&&packet->loss&&packet->bytes==Bytes(8000,2),"test_udp_audio_recovers_reordered_gap");}
    require(priority.retained_bytes()==0,"test_udp_old_partial_reclaimed");
    const auto maximum=shards(2,2,Bytes(192000,3),1400,true);
    for(std::size_t i=maximum.size();i-->0;){auto packet=priority.accept(maximum[i],3);if(!i)require(packet&&packet->bytes==Bytes(192000,3)&&!packet->loss,"test_udp_exact_payload_limit");}
}
