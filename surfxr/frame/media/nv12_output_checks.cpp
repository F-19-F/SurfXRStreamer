#include "decoder_private.hpp"

namespace surfxr {
void nv12_output_self_test(){
    unsigned allocations=0;
    Nv12FrameAllocator allocator=[&](unsigned w,unsigned h){++allocations;return std::make_shared<CpuNv12Storage>(w,h);};
    Nv12FramePool pool(allocator);DecoderStats stats;
    std::array<std::shared_ptr<VideoFrame>,6> held;
    for(auto& f:held)f=pool.acquire(8,4,stats);
    require(allocations==6&&!pool.acquire(8,4,stats),"nv12_pool_must_not_allocate_when_retained");
    const auto released=held[2].get();held[2].reset();
    require(pool.acquire(8,4,stats).get()==released&&allocations==6,"nv12_pool_reuse_failed");
    require(stats.nv12_pool_bytes==6*48,"nv12_pool_budget_incorrect");
    LatestFrameSlot slot;std::uint64_t epoch=1;unsigned callbacks=0;
    VideoDecoder decoder(slot,DecoderBackend::Software,[&](auto){++callbacks;},1,{},[&]{return epoch;},DecoderFrameLayout::PackedStereo,allocator);
    auto& state=*decoder.impl_;auto& input=*state.frame;
    input.width=8;input.height=4;input.format=AV_PIX_FMT_NV12;input.colorspace=AVCOL_SPC_BT709;input.color_range=AVCOL_RANGE_JPEG;
    ffcheck(av_frame_get_buffer(&input,32),"nv12_fixture_allocation");
    for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?2u:4u);++row)for(unsigned x=0;x<8;++x)
        input.data[p][row*input.linesize[p]+x]=std::uint8_t(p*91+row*17+x);
    auto metadata=std::make_shared<VrVideoMetadata>();metadata->timestamp=11;metadata->linear_layout=true;
    const auto publish=[&](std::int64_t pts,VideoColorInfo color={}){input.pts=pts;state.source_stamps.emplace_back(pts,SourceStamp{11,17,metadata,epoch,color});state.publish();
        require(state.source_stamps.empty(),"nv12_exact_stamp_not_consumed");};
    publish(1);auto output=slot.snapshot();
    require(output&&output->nv12_storage&&output->rgba.empty()&&!output->rgba_storage&&output->vr_metadata==metadata&&
        output->source_timestamp==11&&output->packet_submit_monotonic_ns==17&&output->output_epoch==1&&
        output->yuv_coefficients[0]==.2126f&&output->yuv_coefficients[2]==0,"nv12_publication_identity_or_color_changed");
    for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?2u:4u);++row)
        require(std::memcmp(output->nv12_storage->data(p)+row*8,input.data[p]+row*input.linesize[p],8)==0,"nv12_padded_row_copy_incorrect");
    auto leased=std::make_shared<VideoFrame>();leased->nv12_storage=output->nv12_storage;leased->width=8;leased->height=4;
    auto lease=std::make_shared<unsigned>(0);leased->capture_lease=lease;
    const auto detached=*leased;
    require(!detached.capture_lease&&lease.use_count()==2&&detached.nv12_storage!=leased->nv12_storage,
        "nv12_value_copy_retained_capture_lease");
    const auto copy=*output;
    require(copy.nv12_storage!=output->nv12_storage&&copy.nv12_storage->data(1)[8]==108,"nv12_value_copy_alias_or_bytes_error");
    input.data[0][0]=222;publish(2);
    require(output->nv12_storage->data(0)[0]==0&&slot.snapshot()->nv12_storage->data(0)[0]==222,"nv12_reused_held_storage");
    epoch=0;publish(3);require(!slot.snapshot()&&callbacks==2,"nv12_paused_output_published");epoch=2;
    metadata->rgba_deadline_monotonic_ns=1;publish(4);
    require(!slot.snapshot()&&state.stats.nv12_published_frames==2&&state.stats.rgba_converted_frames==0&&
        state.stats.rgba_skipped_frames==2,"nv12_deadline_or_cpu_conversion_gate_failed");
    metadata->rgba_deadline_monotonic_ns=0;input.colorspace=AVCOL_SPC_SMPTE170M;input.color_range=AVCOL_RANGE_MPEG;publish(5);
    require(slot.snapshot()->yuv_coefficients[0]==.299f&&slot.snapshot()->yuv_coefficients[2]==16.f/255.f,"nv12_matrix_range_transition_failed");
    require(nv12_coefficients(AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG)[0]==.2627f&&
        nv12_coefficients(AVCOL_SPC_BT2020_CL,AVCOL_RANGE_JPEG)[0]==0,"nv12_unsupported_matrix_accepted");
    // Exercise the hardware publication policy without opening a device. Iris
    // can label a BT.601 desktop picture as 709; GPU coefficients must use SPS.
    state.stats.backend=DecoderBackend::V4l2;input.colorspace=AVCOL_SPC_BT709;input.color_range=AVCOL_RANGE_JPEG;
    publish(6,{AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG});
    require(slot.snapshot()->yuv_coefficients==nv12_coefficients(AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG)&&
        state.stats.color_space==AVCOL_SPC_BT709&&state.stats.color_range==AVCOL_RANGE_JPEG&&
        state.stats.effective_color_space==AVCOL_SPC_BT470BG&&state.stats.effective_color_range==AVCOL_RANGE_MPEG,
        "nv12_packet_sps_did_not_override_incorrect_device_metadata");
    // Desktop opts into the same GPU output without widening Single's policy.
    VideoDecoder desktop(slot,DecoderBackend::Software,{},1,{}, {},DecoderFrameLayout::Single,allocator);
    require(desktop.impl_->nv12_pool.enabled()&&desktop.impl_->pixel_limit==kMaxPixels,"single_nv12_policy_changed");
    VideoDecoder legacy(slot);
    require(!legacy.impl_->nv12_pool.enabled(),"legacy_rgba_default_changed");
}
}
