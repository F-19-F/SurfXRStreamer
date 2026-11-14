#include "decoder_private.hpp"
#include "nv12_view.hpp"

namespace surfxr {
namespace decoder_detail {
class TestRgbaStorage final:public RgbaFrameStorage {
public:
    explicit TestRgbaStorage(std::size_t bytes):bytes_(bytes),storage_(bytes+128,0x5a){}
    std::uint8_t* data()noexcept override{return storage_.data()+64;}
    std::size_t size()const noexcept override{return bytes_;}
    bool guards_valid()const{return std::all_of(storage_.begin(),storage_.begin()+64,[](auto v){return v==0x5a;})
        &&std::all_of(storage_.end()-64,storage_.end(),[](auto v){return v==0x5a;});}
private:
    std::size_t bytes_;
    std::vector<std::uint8_t> storage_;
};
void rgba_external_storage_self_test(){
    unsigned allocations=0;
    RgbaFrameAllocator allocator=[&](std::size_t bytes){++allocations;return std::make_shared<TestRgbaStorage>(bytes);};
    DecoderStats stats;RgbaFramePool pool(128,allocator);
    auto first=pool.acquire(4,4,stats);first->generation=7;first->output_epoch=11;
    first->packet_submit_monotonic_ns=123;first->conversion_begin_monotonic_ns=456;first->decoder_output_monotonic_ns=345;
    require(first->rgba.empty()&&first->rgba_storage&&first->rgba_size()==64,"rgba_external_storage_not_selected");
    std::fill(first->rgba_data(),first->rgba_data()+first->rgba_size(),std::uint8_t{0xa7});
    const auto* pixels=first->rgba_data();std::weak_ptr<RgbaFrameStorage> storage=first->rgba_storage;
    LatestFrameSlot slot;slot.publish(first);auto gpu_reader=slot.snapshot();first.reset();
    auto second=pool.acquire(4,4,stats);slot.publish(second);
    auto temporary=pool.acquire(4,4,stats);
    require(allocations==2&&second->rgba_storage&&!temporary->rgba_storage&&stats.rgba_temporary_allocations==1,
            "rgba_external_pool_budget_not_bounded");
    require(gpu_reader->rgba_data()==pixels&&gpu_reader->rgba_data()[0]==0xa7,"rgba_external_pool_overwrote_gpu_reader");
    VideoFrame copied=*gpu_reader;VideoFrame assigned;assigned=*gpu_reader;
    require(!copied.rgba_storage&&!assigned.rgba_storage&&copied.rgba.size()==64&&copied.generation==7
            &&copied.rgba.data()!=pixels&&std::memcmp(copied.rgba.data(),pixels,64)==0,"rgba_external_copy_not_independent");
    require(copied.output_epoch==11&&assigned.output_epoch==11&&copied.packet_submit_monotonic_ns==123&&copied.conversion_begin_monotonic_ns==456&&
            assigned.packet_submit_monotonic_ns==123&&assigned.conversion_begin_monotonic_ns==456&&
            copied.decoder_output_monotonic_ns==345&&assigned.decoder_output_monotonic_ns==345,"rgba_copy_lost_decode_timing");
    copied.rgba[0]=0;assigned.rgba[0]=1;
    require(gpu_reader->rgba_data()[0]==0xa7,"rgba_external_copy_mutated_original");
    slot.clear();second.reset();auto reused=pool.acquire(4,4,stats);
    require(reused->rgba_storage&&allocations==2&&stats.rgba_buffer_reuses==1,"rgba_external_pool_failed_reuse");
    pool.clear(stats);reused.reset();temporary.reset();
    require(!storage.expired()&&gpu_reader->rgba_data()[0]==0xa7,"rgba_external_clear_invalidated_gpu_reader");
    gpu_reader.reset();require(storage.expired(),"rgba_external_storage_leaked_after_last_reader");
    DecoderStats fallback_stats;RgbaFramePool fallback(128,[](std::size_t)->std::shared_ptr<RgbaFrameStorage>{return {};});
    const auto ordinary=fallback.acquire(4,4,fallback_stats);
    require(!ordinary->rgba_storage&&ordinary->rgba.size()==64,"rgba_external_allocator_fallback_failed");
    DecoderStats bad_stats;RgbaFramePool bad(128,[](std::size_t bytes){return std::make_shared<TestRgbaStorage>(bytes-1);});
    bool rejected=false;try{bad.acquire(4,4,bad_stats);}catch(const VideoDecodeError&){rejected=true;}
    require(rejected,"rgba_external_short_allocation_accepted");
}
void rgba_pool_self_test(){
    DecoderStats stats;RgbaFramePool pool;LatestFrameSlot slot;
    auto first=pool.acquire(4,4,stats);std::fill(first->rgba.begin(),first->rgba.end(),std::uint8_t{0x5a});
    first->generation=7;first->source_timestamp=123;first->decoded_monotonic_ns=456;first->output_epoch=12;first->has_source_timestamp=true;
    first->packet_submit_monotonic_ns=234;first->conversion_begin_monotonic_ns=345;first->decoder_output_monotonic_ns=300;
    // An ownership-only alias checks that stale opaque metadata is released,
    // without requiring a VR struct definition in this decoder-only module.
    auto metadata_owner=std::make_shared<int>(1);
    first->vr_metadata=std::shared_ptr<const VrVideoMetadata>(metadata_owner,nullptr);
    const auto* identity=first.get();slot.publish(first);auto held=slot.snapshot();first.reset();
    for(unsigned i=0;i<32;++i){
        auto next=pool.acquire(4,4,stats);std::fill(next->rgba.begin(),next->rgba.end(),static_cast<std::uint8_t>(i));
        next->generation=8+i;slot.publish(std::move(next));
        require(held->generation==7&&held->source_timestamp==123&&held->rgba==std::vector<std::uint8_t>(64,0x5a),"rgba_pool_modified_held_frame");
        require(held->output_epoch==12&&held->packet_submit_monotonic_ns==234&&held->conversion_begin_monotonic_ns==345&&held->decoder_output_monotonic_ns==300,"rgba_pool_modified_held_timing");
    }
    require(stats.rgba_buffer_allocations==3&&stats.rgba_buffer_reuses==30&&stats.rgba_pool_bytes==192,"rgba_pool_did_not_reuse_released_storage");
    held.reset();slot.clear();auto reused=pool.acquire(4,4,stats);
    require(reused.get()==identity&&!reused->has_source_timestamp&&reused->source_timestamp==0&&reused->generation==0&&reused->decoded_monotonic_ns==0&&reused->output_epoch==0&&!reused->vr_metadata
            &&reused->packet_submit_monotonic_ns==0&&reused->conversion_begin_monotonic_ns==0&&reused->decoder_output_monotonic_ns==0
            &&metadata_owner.use_count()==1&&reused->rgba==std::vector<std::uint8_t>(64,0x5a),"rgba_pool_reuse_reset_pixels_or_retained_metadata");
    // Exercise frame-count exhaustion while all six published frames are held.
    DecoderStats full_stats;RgbaFramePool full;std::vector<std::shared_ptr<const VideoFrame>> owners;
    for(unsigned i=0;i<6;++i){auto frame=full.acquire(4,4,full_stats);frame->rgba[0]=static_cast<std::uint8_t>(i);owners.push_back(std::move(frame));}
    auto temporary=full.acquire(4,4,full_stats);
    require(full_stats.rgba_pool_buffers==6&&full_stats.rgba_temporary_allocations==1&&full_stats.rgba_buffer_allocations==7,"rgba_pool_frame_cap_failed");
    for(unsigned i=0;i<6;++i)require(owners[i]->rgba[0]==i&&owners[i].get()!=temporary.get(),"rgba_pool_full_mutated_held_frame");
    const auto* released=owners[2].get();owners[2].reset();auto available=full.acquire(4,4,full_stats);
    require(available.get()==released&&full_stats.rgba_buffer_reuses==1,"rgba_pool_failed_reuse_after_full");
    // A small injected byte budget exercises the same policy without allocating
    // hundreds of MiB in a self-test. Dimension changes release only pool owners.
    DecoderStats limited_stats;RgbaFramePool limited(128);
    auto a=limited.acquire(4,4,limited_stats);a->rgba[0]=0xa5;
    auto b=limited.acquire(4,4,limited_stats);auto c=limited.acquire(4,4,limited_stats);
    require(limited_stats.rgba_pool_buffers==2&&limited_stats.rgba_pool_bytes==128&&limited_stats.rgba_temporary_allocations==1,"rgba_pool_byte_cap_failed");
    auto resized=limited.acquire(8,4,limited_stats);
    require(limited_stats.rgba_pool_buffers==1&&limited_stats.rgba_pool_bytes==128&&resized->rgba.size()==128&&a->rgba.size()==64&&a->rgba[0]==0xa5,"rgba_pool_resize_invalidated_old_frame");
    const auto allocations=limited_stats.rgba_buffer_allocations;limited.clear(limited_stats);
    require(limited_stats.rgba_pool_bytes==0&&limited_stats.rgba_pool_buffers==0&&limited_stats.rgba_buffer_allocations==allocations&&a->rgba[0]==0xa5,"rgba_pool_clear_lifetime_or_statistics");
}
void rgba_conversion_self_test(){
    RgbaConverter legacy(1),parallel(4);RgbaFramePool pool;DecoderStats stats;
    RgbaFramePool external_pool(kRgbaPoolBytes,[](std::size_t bytes){return std::make_shared<TestRgbaStorage>(bytes);});DecoderStats external_stats;
    std::shared_ptr<const VideoFrame> external_held;std::vector<std::uint8_t> external_held_pixels;
    struct Configuration {int width,height;AVPixelFormat format;AVColorSpace matrix;AVColorRange range;};
    const Configuration configurations[]{
        {256,144,AV_PIX_FMT_NV12,AVCOL_SPC_BT709,AVCOL_RANGE_MPEG},
        {256,144,AV_PIX_FMT_NV12,AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
        {256,144,AV_PIX_FMT_NV12,AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG},
        {256,144,AV_PIX_FMT_NV12,AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_MPEG},
        {144,256,AV_PIX_FMT_NV12,AVCOL_SPC_BT709,AVCOL_RANGE_MPEG},
        {144,256,AV_PIX_FMT_YUV420P,AVCOL_SPC_BT709,AVCOL_RANGE_MPEG},
        {144,256,AV_PIX_FMT_NV12,AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
        {128,128,AV_PIX_FMT_NV12,AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_MPEG},
        {128,128,AV_PIX_FMT_NV12,AVCOL_SPC_BT709,AVCOL_RANGE_MPEG}};
    std::shared_ptr<const VideoFrame> held;std::vector<std::uint8_t> held_pixels;unsigned phase=0;
    for(const auto& config:configurations){
        Frame input(av_frame_alloc());require(bool(input),"rgba_test_frame_allocation");
        input->width=config.width;input->height=config.height;input->format=config.format;
        input->colorspace=config.matrix;input->color_range=config.range;
        ffcheck(av_frame_get_buffer(input.get(),32),"rgba_test_source_buffer");
        for(unsigned repeat=0;repeat<3;++repeat){
            ++phase;
            for(unsigned plane=0;plane<(config.format==AV_PIX_FMT_NV12?2u:3u);++plane){
                const int rows=plane?config.height/2:config.height;
                const int columns=plane&&config.format!=AV_PIX_FMT_NV12?config.width/2:config.width;
                for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){
                    const auto value=(unsigned(x)*13+unsigned(y)*29+phase*67+plane*43+(unsigned(x)/17)*41)%256;
                    input->data[plane][std::size_t(y)*input->linesize[plane]+x]=static_cast<std::uint8_t>(value);
                }
            }
            VideoFrame reference;reference.width=config.width;reference.height=config.height;reference.stride=reference.width*4;
            reference.rgba.resize(std::size_t{reference.stride}*reference.height);
            auto result=pool.acquire(config.width,config.height,stats);
            const auto refs=av_buffer_get_ref_count(input->buf[0]);
            legacy.convert(*input,reference);parallel.convert(*input,*result);
            require(result->rgba==reference.rgba,"rgba_parallel_pixels_differ");
            if(config.format==AV_PIX_FMT_NV12){
                // Actual borrowed producer helper: padded device stride and
                // UV offset must produce identical RGBA without retaining or
                // modifying the NV12 mapping, including four swscale workers.
                const unsigned stride=config.width+64;
                const auto uv=std::size_t(stride)*(config.height+16);
                std::vector<std::uint8_t> mapped(uv+std::size_t(stride)*(config.height/2),0xcd);
                for(unsigned plane=0;plane<2;++plane)for(int y=0;y<(plane?config.height/2:config.height);++y)
                    std::memcpy(mapped.data()+(plane?uv:0)+std::size_t(y)*stride,
                        input->data[plane]+std::size_t(y)*input->linesize[plane],config.width);
                const auto unchanged=mapped;
                for(auto* converter:{&legacy,&parallel}){
                    Frame view(av_frame_alloc());require(bool(view),"nv12_view_frame_allocation");
                    view->format=AV_PIX_FMT_NV12;view->width=config.width;view->height=config.height;
                    view->colorspace=config.matrix;view->color_range=config.range;
                    consume_nv12_view(*view,mapped.data(),mapped.size(),stride,uv,[&]{
                        require(view->data[0]==mapped.data()&&view->data[1]==mapped.data()+uv,"nv12_view_copied_source");
                        require(!av_buffer_is_writable(view->buf[0]),"nv12_view_not_readonly");
                        converter->convert(*view,*result);
                        require(av_buffer_get_ref_count(view->buf[0])==1,"nv12_view_worker_retained_source");
                    });
                    require(!view->buf[0]&&!view->data[0]&&result->rgba==reference.rgba&&mapped==unchanged,
                        "nv12_view_pixels_or_lifetime_changed");
                    view->format=AV_PIX_FMT_NV12;view->width=config.width;view->height=config.height;
                    rejects([&]{consume_nv12_view(*view,mapped.data(),mapped.size(),stride,uv,
                        []{throw VideoDecodeError("nv12_view_consumer_failure");});});
                    require(!view->buf[0]&&!view->data[0],"nv12_view_exception_kept_mapping");
                    view->format=AV_PIX_FMT_NV12;view->width=config.width;view->height=config.height;
                    rejects([&]{consume_nv12_view(*view,mapped.data(),uv-1,stride,uv,[]{});});
                }
            }
            auto external=external_pool.acquire(config.width,config.height,external_stats);
            for(auto* converter:{&legacy,&parallel}){
                converter->convert(*input,*external);
                require(external->rgba.empty()&&external->rgba_size()==reference.rgba.size()
                        &&std::memcmp(external->rgba_data(),reference.rgba.data(),reference.rgba.size())==0,"rgba_external_conversion_pixels_differ");
                require(std::static_pointer_cast<TestRgbaStorage>(external->rgba_storage)->guards_valid(),"rgba_external_conversion_overran_storage");
            }
            if(!external_held){external_held=external;external_held_pixels=reference.rgba;}
            require(std::memcmp(external_held->rgba_data(),external_held_pixels.data(),external_held_pixels.size())==0,"rgba_external_conversion_overwrote_held_frame");
            require(av_buffer_get_ref_count(input->buf[0])==refs,"rgba_parallel_retained_source");
            for(std::size_t at=3;at<result->rgba.size();at+=4)require(result->rgba[at]==255,"rgba_parallel_alpha_invalid");
            if(phase==1){
                AVFrame unreferenced=*input;unreferenced.buf[0]=nullptr;
                bool rejected=false;try{parallel.convert(unreferenced,*result);}catch(const VideoDecodeError&){rejected=true;}
                require(rejected,"rgba_parallel_accepted_unreferenced_source");
                // The valid conversion immediately following a rejected input
                // must still release all borrowed source/destination references.
                parallel.convert(*input,*result);
                require(result->rgba==reference.rgba&&av_buffer_get_ref_count(input->buf[0])==refs,"rgba_parallel_failure_recovery");
            }
            if(!held){held=result;held_pixels=result->rgba;}
            require(held->rgba==held_pixels,"rgba_parallel_overwrote_held_snapshot");
        }
    }
    require(stats.rgba_buffer_reuses>0,"rgba_parallel_pool_never_reused");
    parallel.reset();legacy.reset();pool.clear(stats);
    external_pool.clear(external_stats);
    require(std::memcmp(external_held->rgba_data(),external_held_pixels.data(),external_held_pixels.size())==0,"rgba_external_reset_invalidated_held_frame");
    require(held&&held->rgba==held_pixels,"rgba_parallel_reset_invalidated_held_snapshot");
}

} // namespace decoder_detail
void nv12_output_self_test();
int video_decoder_self_test(){
    nv12_output_self_test();
    rgba_external_storage_self_test();
    rgba_pool_self_test();
    rgba_conversion_self_test();
    // Reconfiguration may start in the middle of a HEVC reference chain.
    // Actual submit must request IRAP without entering an unopened driver.
    {
        LatestFrameSlot slot;VideoDecoder decoder(slot,DecoderBackend::V4l2);
        auto& state=*decoder.impl_;state.config_codec=2;
        state.config_width=state.config_height=256;state.codec.reset(avcodec_alloc_context3(nullptr));
        const std::uint8_t predicted[]{0,0,0,1,2,1,0xc0};
        const std::uint8_t missing_headers[]{0,0,0,1,38,1,0xa0};
        decoder.submit(predicted,sizeof(predicted),123);
        require(decoder.needs_keyframe()&&state.sequence_pending&&!state.hevc_started&&state.stats.packets==0,
                "hevc_start_admitted_predicted_picture");
        decoder.keyframe_request_queued();decoder.submit(missing_headers,sizeof(missing_headers),124);
        require(!decoder.needs_keyframe()&&state.stats.waiting_for_keyframe_packets==2&&state.source_stamps.empty(),
                "hevc_start_admitted_irap_without_parameter_bank");
        state.hevc_started=true;decoder.deactivate();require(!state.hevc_started,"hevc_deactivate_kept_reference_chain");
    }
    // A busy hardware session loses its reference chain. It cannot replay the
    // rejected input or publish predicted pictures before a new keyframe.
    // Exercise the actual retirement/skip policy without opening a device.
    {
        LatestFrameSlot slot;VideoDecoder decoder(slot,DecoderBackend::V4l2);
        auto& state=*decoder.impl_;
        state.config_codec=1;state.config_width=state.config_height=256;
        state.codec.reset(avcodec_alloc_context3(nullptr));
        state.source_stamps.emplace_back(1,SourceStamp{1,2,{},0,{}});
        require(!state.recover_busy(VideoDecodeError("fixture",AVERROR(EINVAL))),"busy_recovery_swallowed_other_error");
        require(state.recover_busy(VideoDecodeError("fixture",AVERROR(EBUSY)))&&!state.codec&&state.source_stamps.empty()&&
                state.hardware_recovery_pending&&decoder.needs_keyframe()&&!state.stats.failed,"busy_recovery_did_not_retire_reference_chain");
        const std::uint8_t predicted[]{0,0,0,1,0x41,0xe0};
        decoder.submit(predicted,sizeof(predicted),123);
        require(state.stats.packets==0&&state.stats.frames==0&&state.stats.waiting_for_keyframe_packets==1,
                "busy_recovery_admitted_predicted_picture");
        state.codec.reset(avcodec_alloc_context3(nullptr));
        require(state.recover_busy(VideoDecodeError("fixture",AVERROR(EBUSY))),"busy_recovery_second_attempt_missing");
        state.codec.reset(avcodec_alloc_context3(nullptr));
        require(!state.recover_busy(VideoDecodeError("fixture",AVERROR(EBUSY)))&&state.stats.hardware_busy_recoveries==2,
                "busy_recovery_unbounded_retry");
        decoder.configure(1,256,256);
        require(!state.recover_busy(VideoDecodeError("fixture",AVERROR(EBUSY))),"busy_recovery_noop_config_reset_budget");
        decoder.deactivate();
        require(!state.hardware_recovery_pending&&!decoder.needs_keyframe(),"busy_recovery_survived_deactivation");
        LatestFrameSlot software_slot;VideoDecoder software(software_slot);
        software.impl_->codec.reset(avcodec_alloc_context3(nullptr));
        require(!software.impl_->recover_busy(VideoDecodeError("fixture",AVERROR(EBUSY))),"busy_recovery_changed_software_policy");
    }
    // Exercise publication with the actual V4L2 metadata gap without opening a
    // decoder/device. These YCbCr patches use an independent Kr/Kb reference,
    // rather than comparing two calls into the same swscale implementation.
    for(auto backend:{DecoderBackend::Software,DecoderBackend::V4l2})for(unsigned threads:{1u,4u}){
        LatestFrameSlot color_slot;VideoDecoder decoder(color_slot,backend,{},threads);
        auto& state=*decoder.impl_;auto& input=*state.frame;
        input.width=input.height=128;input.format=AV_PIX_FMT_NV12;
        ffcheck(av_frame_get_buffer(&input,32),"color_test_source_buffer");
        const std::array<std::array<unsigned,3>,16> patches{{
            {{16,128,128}},{{235,128,128}},{{64,128,128}},{{128,128,128}},
            {{192,128,128}},{{63,102,240}},{{173,42,26}},{{32,240,118}},
            {{219,16,138}},{{188,154,16}},{{78,214,230}},{{110,80,180}},
            {{160,160,80}},{{0,128,128}},{{255,128,128}},{{128,160,160}}}};
        for(int y=0;y<128;++y)for(int x=0;x<128;++x)
            input.data[0][std::size_t(y)*input.linesize[0]+x]=static_cast<std::uint8_t>(patches[x/8][0]);
        for(int y=0;y<64;++y)for(int x=0;x<128;x+=2){
            input.data[1][std::size_t(y)*input.linesize[1]+x]=static_cast<std::uint8_t>(patches[x/8][1]);
            input.data[1][std::size_t(y)*input.linesize[1]+x+1]=static_cast<std::uint8_t>(patches[x/8][2]);
        }
        struct Case {
            AVColorSpace space;AVColorRange range;VideoColorInfo signaled;
            AVColorSpace hardware_space;AVColorRange hardware_range;
        };
        const Case cases[]{
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED,{},AVCOL_SPC_BT709,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_BT709,AVCOL_RANGE_MPEG,{},AVCOL_SPC_BT709,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_BT709,AVCOL_RANGE_JPEG,{},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_SMPTE170M,AVCOL_RANGE_JPEG,{},AVCOL_SPC_SMPTE170M,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_MPEG,{},AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG,{},AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_JPEG,{},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED,{AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED,{AVCOL_SPC_SMPTE170M,AVCOL_RANGE_MPEG},AVCOL_SPC_SMPTE170M,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED,{AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG},AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_SMPTE170M,AVCOL_RANGE_MPEG,{AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_MPEG,{AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_BT709,AVCOL_RANGE_UNSPECIFIED,{AVCOL_SPC_SMPTE170M,AVCOL_RANGE_JPEG},AVCOL_SPC_SMPTE170M,AVCOL_RANGE_JPEG},
            // Actual Iris desktop output reports 709 despite the BT.601 SPS.
            {AVCOL_SPC_BT709,AVCOL_RANGE_MPEG,{AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG},AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_BT709,AVCOL_RANGE_JPEG,{AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG},AVCOL_SPC_BT470BG,AVCOL_RANGE_MPEG},
            {AVCOL_SPC_BT709,AVCOL_RANGE_MPEG,{AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_JPEG},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_SMPTE170M,AVCOL_RANGE_JPEG,{AVCOL_SPC_BT709,AVCOL_RANGE_UNSPECIFIED},AVCOL_SPC_BT709,AVCOL_RANGE_JPEG},
            {AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED,{},AVCOL_SPC_BT709,AVCOL_RANGE_MPEG}};
        constexpr auto count=sizeof(cases)/sizeof(cases[0]);
        const auto pts=[](unsigned index){return std::int64_t{100+(index^1u)};};
        // Queue all packet colors before observing reordered output. The final
        // frame is deliberately unmatched despite another full-range stamp.
        for(unsigned i=0;i+1<count;++i)state.source_stamps.emplace_back(pts(i),SourceStamp{i+1,i+2,{},0,cases[i].signaled});
        state.source_stamps.emplace_back(999,SourceStamp{999,999,{},0,{AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG}});
        std::shared_ptr<const VideoFrame> first;std::vector<std::uint8_t> first_pixels;
        for(unsigned index=0;index<count;++index){
            const auto& test=cases[index];av_buffer_unref(&input.opaque_ref);input.pts=pts(index);
            if(index%2&&index+1<count){
                auto* stamp=new SourceStamp{index+1,index+2,{},0,test.signaled};
                input.opaque_ref=av_buffer_create(reinterpret_cast<std::uint8_t*>(stamp),sizeof(SourceStamp),
                    [](void*,std::uint8_t* bytes){delete reinterpret_cast<SourceStamp*>(bytes);},nullptr,AV_BUFFER_FLAG_READONLY);
                if(!input.opaque_ref)delete stamp;
                require(input.opaque_ref,"color_test_stamp_allocation");
                // An exact opaque stamp takes precedence over even a conflicting
                // PTS-map entry, matching the production metadata policy.
                for(auto& pending:state.source_stamps)if(pending.first==input.pts)pending.second.color={AVCOL_SPC_BT2020_NCL,AVCOL_RANGE_JPEG};
            }
            input.colorspace=test.space;input.color_range=test.range;state.publish();
            const auto result=color_slot.snapshot();require(bool(result),"color_test_frame_missing");
            const auto expected_space=backend==DecoderBackend::V4l2?test.hardware_space:
                (test.space==AVCOL_SPC_UNSPECIFIED?AVCOL_SPC_SMPTE170M:test.space);
            const bool full=(backend==DecoderBackend::V4l2?test.hardware_range:test.range)==AVCOL_RANGE_JPEG;
            require(state.stats.color_space==test.space&&state.stats.color_range==test.range&&
                    input.colorspace==test.space&&input.color_range==test.range,"color_test_raw_metadata_modified");
            require(state.stats.effective_color_space==expected_space&&
                    state.stats.effective_color_range==(full?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG),"color_test_effective_metadata");
            const double kr=expected_space==AVCOL_SPC_BT709?.2126:expected_space==AVCOL_SPC_BT2020_NCL?.2627:.299;
            const double kb=expected_space==AVCOL_SPC_BT709?.0722:expected_space==AVCOL_SPC_BT2020_NCL?.0593:.114;
            for(unsigned i=0;i<patches.size();++i){
                const auto& patch=patches[i];
                const double y=(static_cast<double>(patch[0])-(full?0:16))/(full?255:219);
                const double cb=(static_cast<double>(patch[1])-128)/(full?255:224);
                const double cr=(static_cast<double>(patch[2])-128)/(full?255:224);
                const double r=y+2*(1-kr)*cr,b=y+2*(1-kb)*cb;
                const double g=(y-kr*r-kb*b)/(1-kr-kb);
                const double expected[]{r,g,b};
                // Sample inside each constant chroma block, away from the
                // bilinear boundary. Integer/SIMD swscale rounding is bounded.
                const auto* actual=result->rgba_data()+64*result->stride+(i*8+4)*4;
                for(unsigned c=0;c<3;++c){
                    const int reference=static_cast<int>(std::lround(std::clamp(expected[c],0.0,1.0)*255));
                    require(std::abs(static_cast<int>(actual[c])-reference)<=3,"color_test_independent_matrix_or_range_mismatch");
                }
                require(actual[3]==255,"color_test_alpha_not_opaque");
            }
            if(!first){first=result;first_pixels.assign(result->rgba_data(),result->rgba_data()+result->rgba_size());}
            require(std::memcmp(first->rgba_data(),first_pixels.data(),first_pixels.size())==0,"color_test_transition_mutated_published_frame");
        }
        require(state.source_stamps.size()==1&&state.source_stamps.front().first==999&&state.stats.metadata_unmatched==1,
                "color_test_packet_metadata_not_consumed_exactly");
    }
    // Exercise the real publication/conversion path with refcounted NV12,
    // without a codec/device. Epochs follow reordered PTS or opaque metadata;
    // pause still consumes exact stamps and advances decoded generations.
    for(unsigned threads:{1u,4u})for(bool external:{false,true}){
        LatestFrameSlot gated_slot;std::uint64_t epoch=1;unsigned allocations=0,callbacks=0;
        bool change_during_allocation=false;
        RgbaFrameAllocator allocator=[&](std::size_t bytes)->std::shared_ptr<RgbaFrameStorage>{
            ++allocations;if(change_during_allocation){epoch=4;change_during_allocation=false;}
            return external?std::make_shared<TestRgbaStorage>(bytes):std::shared_ptr<RgbaFrameStorage>{};
        };
        VideoDecoder gated(gated_slot,DecoderBackend::Software,[&](auto){++callbacks;},threads,allocator,[&]{return epoch;});
        auto& state=*gated.impl_;auto& input=*state.frame;
        input.width=input.height=128;input.format=AV_PIX_FMT_NV12;
        input.colorspace=AVCOL_SPC_BT709;input.color_range=AVCOL_RANGE_MPEG;
        ffcheck(av_frame_get_buffer(&input,32),"demand_test_source_buffer");
        for(unsigned plane=0;plane<2;++plane)for(int row=0;row<(plane?64:128);++row)
            for(int col=0;col<128;++col)input.data[plane][std::size_t(row)*input.linesize[plane]+col]=
                static_cast<std::uint8_t>((row*17+col*13+plane*97)&255);
        auto metadata_owner=std::make_shared<int>(5);
        const std::shared_ptr<const VrVideoMetadata> metadata(metadata_owner,nullptr);
        const auto output=[&](std::int64_t pts,std::uint64_t submitted_epoch,bool opaque=false){
            av_buffer_unref(&input.opaque_ref);input.pts=pts;
            const SourceStamp exact{static_cast<std::uint64_t>(pts)+1000,static_cast<std::uint64_t>(pts)+2000,metadata,submitted_epoch};
            state.source_stamps.emplace_back(pts,exact);
            if(opaque){auto* stamp=new SourceStamp(exact);
                input.opaque_ref=av_buffer_create(reinterpret_cast<std::uint8_t*>(stamp),sizeof(SourceStamp),
                    [](void*,std::uint8_t* bytes){delete reinterpret_cast<SourceStamp*>(bytes);},nullptr,AV_BUFFER_FLAG_READONLY);
                if(!input.opaque_ref)delete stamp;
                require(input.opaque_ref,"demand_test_stamp_allocation");
                // An exact opaque stamp takes precedence, never a newer map
                // entry or a different demand epoch with the same PTS.
                state.source_stamps.back().second.output_epoch=0;
            }
            state.publish();require(state.source_stamps.empty(),"demand_left_consumed_metadata");
        };
        output(11,1);auto held=gated_slot.snapshot();
        require(held&&held->generation==1&&held->output_epoch==1&&held->source_timestamp==1011&&
                held->packet_submit_monotonic_ns==2011&&!held->vr_metadata.owner_before(metadata)&&
                !metadata.owner_before(held->vr_metadata),"demand_initial_identity_changed");
        const std::vector<std::uint8_t> held_pixels(held->rgba_data(),held->rgba_data()+held->rgba_size());
        const auto conversion_ns=state.stats.rgba_conversion_time_ns;
        epoch=0;output(13,1,true);
        require(!gated_slot.snapshot()&&allocations==1&&callbacks==1&&state.stats.frames==2&&
                state.stats.rgba_skipped_frames==1&&state.stats.rgba_conversion_time_ns==conversion_ns,
                "demand_pause_allocated_converted_or_published");
        epoch=2;output(12,1);output(14,0);
        require(!gated_slot.snapshot()&&allocations==1&&state.stats.frames==4&&state.stats.rgba_skipped_frames==3,
                "demand_resume_accepted_old_or_paused_packet");
        output(15,2,true);auto resumed=gated_slot.snapshot();
        require(resumed&&resumed->generation==5&&resumed->output_epoch==2&&resumed->source_timestamp==1015&&
                std::memcmp(resumed->rgba_data(),held_pixels.data(),held_pixels.size())==0,
                "demand_resume_changed_pixels_or_generation");
        epoch=3;change_during_allocation=true;output(16,3);
        require(!change_during_allocation&&!gated_slot.snapshot()&&callbacks==2&&
                state.stats.rgba_converted_frames==3&&state.stats.rgba_suppressed_publications==1,
                "demand_conversion_race_was_published");
        output(17,4);
        require(gated_slot.snapshot()&&gated_slot.snapshot()->generation==7&&gated_slot.snapshot()->output_epoch==4&&
                callbacks==3&&state.stats.frames==7&&state.stats.rgba_converted_frames==4&&state.stats.rgba_skipped_frames==3&&
                state.stats.metadata_from_pts==5&&state.stats.metadata_from_opaque==2&&state.stats.metadata_unmatched==0,
                "demand_decoded_converted_metadata_accounting");
        require(held->output_epoch==1&&held->generation==1&&
                std::memcmp(held->rgba_data(),held_pixels.data(),held_pixels.size())==0,
                "demand_pause_mutated_retained_frame");
        const auto current=gated_slot.snapshot();const auto before_deadline=state.stats.rgba_conversion_time_ns;
        auto expired=std::make_shared<VrVideoMetadata>();expired->rgba_deadline_monotonic_ns=1;
        input.pts=18;state.source_stamps.emplace_back(18,SourceStamp{1018,2018,expired,epoch});state.publish();
        require(gated_slot.snapshot()==current&&state.source_stamps.empty()&&callbacks==3&&state.stats.frames==8&&
                state.stats.rgba_skipped_frames==4&&state.stats.rgba_conversion_time_ns==before_deadline,
                "expired_rgba_work_changed_references_or_published_frame");
        auto fresh=std::make_shared<VrVideoMetadata>();fresh->rgba_deadline_monotonic_ns=monotonic_ns()+1'000'000'000;
        input.pts=19;state.source_stamps.emplace_back(19,SourceStamp{1019,2019,fresh,epoch});state.publish();
        require(gated_slot.snapshot()->generation==9&&gated_slot.snapshot()->vr_metadata==fresh&&callbacks==4&&
                state.stats.rgba_converted_frames==5&&state.source_stamps.empty(),"fresh_rgba_deadline_not_published");
        auto ordinary=std::make_shared<VrVideoMetadata>();
        input.pts=20;state.source_stamps.emplace_back(20,SourceStamp{1020,2020,ordinary,epoch});state.publish();
        require(gated_slot.snapshot()->generation==10&&callbacks==5&&state.stats.rgba_converted_frames==6,
                "default_rgba_deadline_changed_publication");
        gated.deactivate();require(!gated_slot.snapshot()&&held->output_epoch==1&&
                std::memcmp(held->rgba_data(),held_pixels.data(),held_pixels.size())==0,
                "demand_deactivate_invalidated_retained_storage");
    }
    LatestFrameSlot slot;require(!slot.snapshot(),"slot_not_empty");
    auto first=std::make_shared<VideoFrame>();first->generation=1;first->rgba={1,2,3,4};
    slot.publish(first);auto held=slot.snapshot();
    auto second=std::make_shared<VideoFrame>();second->generation=2;second->rgba={5,6,7,8};
    slot.publish(second);require(slot.snapshot()->generation==2 && held->rgba[0]==1,"slot_snapshot_lifetime_failed");
    slot.clear();require(!slot.snapshot() && held->generation==1,"slot_clear_failed");
    VideoDecoder no_config(slot);const std::uint8_t byte=0;
    require(no_config.poll()==0&&!no_config.statistics().failed,"poll_unconfigured_failed");
    rejects([&]{no_config.submit(&byte,1,0);});rejects([&]{no_config.configure(1,64,64);});
    rejects([&]{no_config.poll();});
    no_config.deactivate();require(no_config.statistics().failed,"deactivate_cleared_failure");
    rejects([&]{no_config.configure(1,64,64);});
    VideoDecoder inactive(slot);inactive.deactivate();inactive.deactivate();
    require(inactive.poll()==0&&inactive.statistics().packets==0&&inactive.statistics().frames==0,"poll_inactive_changed_frames");
    require(!slot.snapshot() && !inactive.statistics().failed && inactive.statistics().configuration_changes==0,"idle_deactivate_failed");
    VideoDecoder bad_codec(slot);rejects([&]{bad_codec.configure(0,64,64);});
    VideoDecoder bad_size(slot);rejects([&]{bad_size.configure(1,0xffffffffu,0xffffffffu);});
    VideoDecoder finished(slot);finished.finish();finished.finish();require(finished.poll()==0,"poll_finished_failed");
    finished.deactivate();rejects([&]{finished.configure(1,64,64);});
    require(std::strcmp(decoder_backend_name(DecoderBackend::Software),"software")==0 &&
            std::strcmp(decoder_backend_name(DecoderBackend::V4l2),"v4l2")==0,"backend_name_failed");
    rejects([&]{VideoDecoder invalid(slot,static_cast<DecoderBackend>(-1));});
    for(const unsigned threads:{0u,2u,5u})rejects([&]{VideoDecoder invalid(slot,DecoderBackend::Software,{},threads);});
    VideoDecoder parallel(slot,DecoderBackend::Software,{},4);
    require(parallel.statistics().rgba_conversion_threads==4&&inactive.statistics().rgba_conversion_threads==1,"rgba_thread_selection_failed");
    // All hardware-backend negative tests fail before avcodec_open2: they never
    // open a V4L2 device or depend on a usable hardware decoder being installed.
    require(std::strcmp(v4l2_decoder_name(1),"h264_v4l2m2m")==0
        &&std::strcmp(v4l2_decoder_name(2),"hevc_v4l2m2m")==0,"hardware_codec_mapping_failed");
    for(const auto code:{0u,3u,4u,5u,6u,10u,11u}){
        VideoDecoder hardware(slot,DecoderBackend::V4l2);
        rejects([&]{hardware.configure(code,256,256);});
        const auto state=hardware.statistics();
        require(state.failed && state.backend==DecoderBackend::V4l2 &&
                std::strcmp(state.selected_decoder,"none")==0,"hardware_rejection_state_failed");
        rejects([&]{hardware.configure(2,256,256);});
    }
    VideoDecoder hardware_size(slot,DecoderBackend::V4l2);
    rejects([&]{hardware_size.configure(2,0,256);});
    VideoDecoder single_limit(slot);
    rejects([&]{single_limit.configure(2,6144,3264);});
    VideoDecoder packed(slot,DecoderBackend::Software,{},1,{},{},DecoderFrameLayout::PackedStereo);
    packed.configure(2,6144,3264);
    require(packed.statistics().configuration_changes==1,"packed_godlike_configuration_rejected");
    rejects([&]{packed.configure(2,8192,8192);});
    rejects([&]{VideoDecoder invalid(slot,DecoderBackend::Software,{},1,{},{},static_cast<DecoderFrameLayout>(-1));});
    return 0;
}
} // namespace surfxr
