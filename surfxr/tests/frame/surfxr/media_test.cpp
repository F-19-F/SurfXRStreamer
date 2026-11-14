#include "wire.hpp"
#include "tracking_ledger.hpp"
#include "surfxr/stereo_splitter.hpp"
#include "surfxr/haptics.hpp"
#include "surfxr/surfxr_feedback.hpp"
#include "surfxr/vr_frame.hpp"
#include <cmath>

using namespace surfxr::transport;
namespace {
class PackedStorage final:public surfxr::RgbaFrameStorage {
    std::array<std::uint8_t,32> bytes_{};
public:
    std::uint8_t* data()noexcept override{return bytes_.data();}
    std::size_t size()const noexcept override{return bytes_.size();}
};
}
void media_checks(){
    surfxr::TrackingFrame frame;frame.sequence=7;frame.origin_epoch=1;frame.head.flags=surfxr::OrientationValid|surfxr::PositionValid;
    frame.head.pose.position={1,2,3};frame.optics_valid=true;frame.ipd_meters=.064f;
    frame.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f}}};
    const auto bytes=tracking(frame,1'000'000'234,false);Reader r(bytes);
    // bincode::serialize(surfxr_packets::Tracking), upstream v20.14.1 Rust oracle.
    const std::string golden="0100000000000000ea0000000100000000000000382520c93d85905b0000000000000000000000000000803f0000803f000000400000404000000000000000000000000000000000000000000000000000000000000000";
    std::string actual;constexpr char hex[]="0123456789abcdef";for(auto b:bytes){actual+=hex[b>>4];actual+=hex[b&15];}
    require(actual==golden,"test_upstream_bincode_bytes");
    require(r.duration()==1'000'000'234&&r.integer(8)==1&&r.integer(8)==path_id("/user/head"),"test_tracking_header");
    require(r.real()==0&&r.real()==0&&r.real()==0&&r.real()==1,"test_tracking_quaternion");
    require(r.real()==1&&r.real()==2&&r.real()==3,"test_tracking_position");
    for(unsigned i=0;i<6;++i)require(r.real()==0,"test_predicted_head_zero_velocities");
    for(unsigned i=0;i<7;++i)require(!r.boolean(),"test_tracking_extensions");
    r.end();
    TrackingLedger ledger;ledger.sent(100,frame);frame.head.pose.position.x=9;ledger.sent(200,frame);
    const auto meta=ledger.metadata(100,32,32,300);
    require(meta&&std::abs(meta->eye_poses[0].position.x-.968f)<.0001f&&meta->linear_layout,"test_exact_sent_pose");
    require(!ledger.metadata(101,32,32,300),"test_no_latest_pose_substitution");
    const auto new_image=ledger.metadata(100,32,32,300,250);
    const auto next_image=ledger.metadata(100,32,32,310,260);
    require(new_image&&next_image&&new_image->timestamp==250&&next_image->timestamp==260&&
        new_image->surfxr_tracking_timestamp==100&&next_image->tracking_frame_id==meta->tracking_frame_id&&
        new_image->eye_poses[0].position.x==meta->eye_poses[0].position.x&&
        new_image->rgba_deadline_monotonic_ns==meta->rgba_deadline_monotonic_ns,"test_new_images_keep_exact_old_pose_and_freshness");
    frame.origin_epoch=2;ledger.sent(400,frame);require(!ledger.metadata(100,32,32,500),"test_origin_change");
    surfxr::VideoFrame packed;packed.width=4;packed.height=2;packed.stride=20;packed.generation=4;packed.source_timestamp=100;
    packed.has_source_timestamp=true;packed.vr_metadata=meta;packed.output_epoch=8;packed.rgba.resize(40);
    for(std::size_t i=0;i<packed.rgba.size();++i)packed.rgba[i]=static_cast<std::uint8_t>(i);
    surfxr::StereoSplitter splitter;auto eyes=splitter.split(packed);
    require(eyes[0]->width==2&&eyes[0]->stride==8&&eyes[0]->output_epoch==8&&eyes[1]->vr_metadata==meta,"test_split_metadata");
    require(eyes[0]->rgba[8]==20&&eyes[1]->rgba[0]==8&&eyes[1]->rgba[8]==28,"test_row_pitch_and_eye_order");
    const auto saved=eyes[0]->rgba;std::array<std::array<std::shared_ptr<const surfxr::VideoFrame>,2>,5> held;
    for(auto& pair:held)pair=splitter.split(packed);
    require(!splitter.split(packed)[0]&&eyes[0]->rgba==saved,"test_bounded_pool_ownership");
    held[0]={};require(splitter.split(packed)[0]!=nullptr,"test_pool_reuse_after_release");
    auto owned=std::make_shared<surfxr::VideoFrame>();owned->width=4;owned->height=2;owned->stride=16;
    owned->generation=4;owned->source_timestamp=100;owned->has_source_timestamp=true;owned->vr_metadata=meta;owned->output_epoch=8;
    owned->rgba_storage=std::make_shared<PackedStorage>();
    for(unsigned i=0;i<32;++i)owned->rgba_data()[i]=static_cast<std::uint8_t>(i);
    auto views=splitter.split(owned);
    require(views[0]->packed_source==owned&&views[1]->packed_source==owned&&views[1]->packed_eye==1&&
        views[0]->rgba.empty()&&!views[0]->rgba_storage,"test_shared_packed_ownership");
    require(surfxr::valid_vr_video_frame(views[0],*meta)&&surfxr::valid_vr_video_frame(views[1],*meta),"test_packed_presentation_validation");
    const surfxr::VideoFrame copied=*views[1];
    require(!copied.packed_source&&!copied.rgba_storage&&copied.rgba[0]==8&&copied.rgba[8]==24,"test_eye_view_value_copy");
    std::weak_ptr<const surfxr::VideoFrame> retained=owned;owned.reset();views[0].reset();
    require(!retained.expired(),"test_right_eye_retains_packed_pixels");views[1].reset();
    require(retained.expired(),"test_packed_source_released_after_both_eyes");
    auto atlas_meta=std::make_shared<surfxr::VrVideoMetadata>(*meta);atlas_meta->linear_layout=false;atlas_meta->gaze_atlas={32,32,16,16};
    atlas_meta->render_width=atlas_meta->render_height=96;
    auto atlas=std::make_shared<surfxr::VideoFrame>();atlas->width=64;atlas->height=48;atlas->stride=64*4;
    atlas->generation=4;atlas->source_timestamp=100;atlas->has_source_timestamp=true;atlas->vr_metadata=atlas_meta;
    atlas->rgba.resize(atlas->stride*atlas->height);
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x)atlas->rgba[y*atlas->stride+x*4]=std::uint8_t(x+y);
    const auto tiles=splitter.split_gaze(atlas);
    for(const auto& tile:tiles)require(tile->packed_source==atlas&&surfxr::valid_vr_video_frame(tile,*atlas_meta),"test_gaze_view_ownership_and_validity");
    const surfxr::VideoFrame focus_copy=*tiles[3];
    require(focus_copy.width==16&&focus_copy.height==16&&focus_copy.rgba[0]==64&&focus_copy.rgba[16*4]==65,"test_gaze_focus_copy_offsets");
    atlas=std::make_shared<surfxr::VideoFrame>(*atlas);atlas->rgba.clear();atlas->nv12_storage=std::make_shared<surfxr::CpuNv12Storage>(64,48);
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x)atlas->nv12_storage->data(0)[y*64+x]=std::uint8_t(x+y);
    const auto nv12_tiles=splitter.split_gaze(atlas);const surfxr::VideoFrame nv12_copy=*nv12_tiles[3];
    require(nv12_copy.nv12_storage&&nv12_copy.nv12_storage->data(0)[0]==64&&nv12_copy.nv12_storage->data(0)[16]==65,"test_gaze_nv12_copy_offsets");
    surfxr::VrStereoFrame gaze_pair;gaze_pair.origin_epoch=1;gaze_pair.foveated=true;gaze_pair.metadata=atlas_meta;
    gaze_pair.reconstruction_width=96;gaze_pair.reconstruction_height=96;gaze_pair.eyes={nv12_tiles[0],nv12_tiles[2]};gaze_pair.focus={nv12_tiles[1],nv12_tiles[3]};
    require(surfxr::valid_vr_stereo_frame(gaze_pair,1)&&!surfxr::valid_vr_stereo_frame(gaze_pair,2),"test_complete_gaze_atlas_epoch");
    std::swap(gaze_pair.focus[0],gaze_pair.focus[1]);require(!surfxr::valid_vr_stereo_frame(gaze_pair,1),"test_gaze_swapped_focus_rejected");
    std::swap(gaze_pair.focus[0],gaze_pair.focus[1]);
    gaze_pair.focus[1].reset();require(!surfxr::valid_vr_stereo_frame(gaze_pair,1),"test_no_incomplete_gaze_publication");
    surfxr::HapticsQueue queue;require(queue.ingest_pulse(1,.5f,75'000'000,150,1000),"test_pulse_ingestion");
    auto commands=queue.consume(2000,true);require(commands[1].changed&&commands[1].duration_ns==75'000'000&&commands[1].frequency==150,"test_explicit_haptics");
    require(!queue.ingest_pulse(2,.5f,1,0,2000),"test_haptic_hand_limit");
    queue.consume(3000,false);require(!queue.ingest_pulse(0,.5f,1,0,4000),"test_unfocused_haptics_discarded");
    surfxr::HapticsQueue source_haptics;
    require(source_haptics.ingest_video_amplitudes(7,1,{.4f,.7f},1'000'000'000),"test_source_amplitudes_ingested");
    auto source_commands=source_haptics.consume(1'001'000'000,true);
    require(source_commands[0].amplitude==.4f&&source_commands[1].amplitude==.7f&&source_commands[0].duration_ns==30'000'000,"test_source_gain_not_applied_twice");
    require(!source_haptics.ingest_video_amplitudes(7,1,{.4f,.7f},1'020'000'000),"test_source_duplicate_not_replayed");
    require(!source_haptics.ingest_video_amplitudes(6,1,{1,1},1'020'000'000),"test_source_old_not_replayed");
    require(source_haptics.consume(1'031'000'000,true)[0].amplitude==0,"test_source_stall_finite_stop");
    source_haptics.consume(1'040'000'000,false);source_haptics.ingest_video_amplitudes(8,1,{1,1},1'041'000'000);
    require(!source_haptics.consume(1'042'000'000,true)[0].changed,"test_source_focus_return_no_replay");
    surfxr::VrStereoFrame pair;pair.metadata=meta;pair.eyes=eyes;
    FeedbackQueue feedback;feedback.presented(pair,1000);feedback.presented(pair,2000);
    Feedback value;require(feedback.pop(value)&&value.timestamp==100&&value.total==900&&!feedback.pop(value),"test_unique_submission_feedback");
    auto gaze_meta=std::make_shared<surfxr::VrVideoMetadata>(*meta);gaze_meta->linear_layout=false;gaze_meta->gaze_atlas={32,32,16,16};
    gaze_meta->timestamp=101;gaze_meta->surfxr_tracking_timestamp=101;pair.metadata=gaze_meta;feedback.presented(pair,2000);
    require(feedback.pop(value)&&value.timestamp==101,"test_gaze_atlas_submission_feedback");
    auto captured_meta=std::make_shared<surfxr::VrVideoMetadata>(*gaze_meta);captured_meta->timestamp=250;
    pair.metadata=captured_meta;feedback.presented(pair,2100);
    require(!feedback.pop(value),"test_new_capture_does_not_duplicate_tracking_ack");
    captured_meta=std::make_shared<surfxr::VrVideoMetadata>(*captured_meta);
    captured_meta->timestamp=260;captured_meta->surfxr_tracking_timestamp=102;
    pair.metadata=captured_meta;feedback.presented(pair,2200);
    require(feedback.pop(value)&&value.timestamp==102&&value.total==2098,"test_capture_ack_uses_original_tracking_time");
}
