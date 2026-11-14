#include "foveation_gpu_test_cases.hpp"
namespace surfxr::tests::foveation_gpu {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}

void check(VkResult result,const char* message){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(message)+"_"+std::to_string(result));}

double original_coordinate(double p){
    if(p<1.0/8)return std::sqrt(p/2);
    if(p<7.0/8)return (p-1.0/8)*2/3+1.0/4;
    return 1-std::sqrt((1-p)/2);
}

std::uint8_t encode_srgb(double linear){const double s=linear<=.0031308?12.92*linear:1.055*std::pow(linear,1/2.4)-.055;return static_cast<std::uint8_t>(std::lround(std::clamp(s,0.0,1.0)*255));}

double decode_srgb(std::uint8_t byte){const double s=byte/255.0;return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);}

std::array<double,3> grid(double u,double v,unsigned eye){
    for(unsigned i=0;i<markers.size();++i)if(std::abs(u-markers[i][0])<.026&&std::abs(v-markers[i][1])<.026)return colors[(i+eye)%colors.size()];
    for(unsigned i=1;i<8;++i)if(std::abs(u-i/8.0)<.006||std::abs(v-i/8.0)<.006)return {.01,.01,.01};
    return {.3+.3*u,.3+.3*v,.32};
}

std::vector<std::uint8_t> encoded_fixture(unsigned width,unsigned height,bool use_grid,bool linear_layout){
    const auto eye_bytes=std::size_t{width}*height*4;std::vector<std::uint8_t> result(eye_bytes*2);
    for(unsigned eye=0;eye<2;++eye)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const double u=linear_layout?(x+.5)/width:original_coordinate((x+.5)/width),v=linear_layout?(y+.5)/height:original_coordinate((y+.5)/height);
        const auto color=use_grid?grid(u,v,eye):std::array<double,3>{{u,v,eye?.72:.18}};
        auto* pixel=result.data()+eye_bytes*eye+(std::size_t{y}*width+x)*4;
        for(unsigned c=0;c<3;++c)pixel[c]=encode_srgb(color[c]);
        pixel[3]=255;
    }return result;
}

std::array<double,3> pixel_at(const std::vector<std::uint8_t>& pixels,unsigned width,unsigned height,unsigned eye,double u,double v){
    const auto x=std::min(static_cast<unsigned>(u*width),width-1),y=std::min(static_cast<unsigned>(v*height),height-1);
    const auto* p=pixels.data()+(std::size_t{eye}*width*height+std::size_t{y}*width+x)*4;
    require(p[3]==255,"nonopaque_output");return {decode_srgb(p[0]),decode_srgb(p[1]),decode_srgb(p[2])};
}

unsigned parse_size(const char* value,unsigned minimum){unsigned result=0;require(*value!=0,"empty_dimension");for(const char* p=value;*p;++p){require(*p>='0'&&*p<='9'&&result<=819,"invalid_dimension");result=result*10+static_cast<unsigned>(*p-'0');}require(result>=minimum&&result<=8192,"dimension_out_of_range");return result;}

void run(unsigned sw,unsigned sh,unsigned width,unsigned height,bool linear_layout){
    Vulkan vk;vk.initialize(linear_layout);
    for(auto& image:vk.sources)vk.image(image,sw,sh,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.upload,std::size_t{sw}*sh*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,std::size_t{width}*height*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    surfxr::FoveationRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,format,width,height);
    std::array<surfxr::FoveationEye,2> eyes{};for(unsigned eye=0;eye<2;++eye)eyes[eye]={vk.sources[eye].image,vk.sources[eye].view,vk.targets[eye].view,true};
    for(auto& eye:eyes)eye.linear_layout=linear_layout;
    vk.upload_pixels(encoded_fixture(sw,sh,false,linear_layout),sw,sh,true);require(renderer.render(eyes),"coordinate_render_skipped");
    double max_error=0,border_error=0;unsigned coordinate_checks=0;const auto coordinates=vk.read_pixels(width,height);
    for(std::size_t i=3;i<coordinates.size();i+=4)require(coordinates[i]==255,"full_target_not_opaque");
    // Clamp-to-edge cannot reconstruct beyond the original coordinates sampled
    // by the first/last encoded texel centers. Report that finite-resolution
    // loss separately; do not weaken the interior coordinate tolerance.
    const double min_u=linear_layout?.5/sw:original_coordinate(.5/sw),min_v=linear_layout?.5/sh:original_coordinate(.5/sh);
    for(unsigned eye=0;eye<2;++eye)for(double u:{0.0,1.0})for(double v:{0.0,1.0}){
        const auto actual=pixel_at(coordinates,width,height,eye,u,v);
        const double expected_u=u?1-min_u:min_u,expected_v=v?1-min_v:min_v;
        require(std::abs(actual[0]-expected_u)<.009&&std::abs(actual[1]-expected_v)<.009,"border_clamp_or_uv_orientation_error");
        border_error=std::max({border_error,std::abs(actual[0]-u),std::abs(actual[1]-v)});
    }
    for(unsigned eye=0;eye<2;++eye)for(double u:{.04,.08,.125,.2,.25,.375,.5,.625,.75,.8,.875,.92,.96})for(double v:{.05,.125,.22,.25,.375,.5,.625,.75,.81,.875,.95}){
        const auto actual=pixel_at(coordinates,width,height,eye,u,v);
        const double eu=(std::min(static_cast<unsigned>(u*width),width-1)+.5)/width,ev=(std::min(static_cast<unsigned>(v*height),height-1)+.5)/height;
        const std::array<double,3> expected{{eu,ev,eye?.72:.18}};
        for(unsigned c=0;c<3;++c){const double error=std::abs(actual[c]-expected[c]);max_error=std::max(max_error,error);require(error<.014,"coordinate_field_mapping_or_srgb_error");}
        ++coordinate_checks;
    }
    for(auto& eye:eyes)eye.dirty=false;
    require(!renderer.render(eyes),"clean_frame_must_skip_gpu");require(vk.read_pixels(width,height)==coordinates,"clean_frame_changed_output");
    vk.upload_pixels(encoded_fixture(sw,sh,true,linear_layout),sw,sh,false);eyes[0].dirty=true;
    require(renderer.render(eyes,true),"single_eye_render_skipped");
    require(renderer.last_gpu_wait_ms()==0,"deferred_render_waited_on_host");
    // This readback submits and waits on the same queue, covering the deferred
    // reconstruction exactly as the later enhancement pass does in the viewer.
    const auto partial=vk.read_pixels(width,height);renderer.complete(true);
    require(renderer.last_gpu_wait_ms()==0,"covered_render_waited_again");
    const auto eye_bytes=std::size_t{width}*height*4;
    require(std::equal(coordinates.begin()+eye_bytes,coordinates.end(),partial.begin()+eye_bytes),"clean_right_eye_was_overwritten");
    eyes[0].dirty=false;eyes[1].dirty=true;require(renderer.render(eyes,true),"second_eye_render_skipped");
    renderer.complete(false);const auto pixels=vk.read_pixels(width,height);
    require(std::equal(partial.begin(),partial.begin()+eye_bytes,pixels.begin()),"clean_left_eye_was_overwritten");
    unsigned marker_checks=0,grid_checks=0;
    for(unsigned eye=0;eye<2;++eye){
        for(unsigned i=0;i<markers.size();++i){const auto actual=pixel_at(pixels,width,height,eye,markers[i][0],markers[i][1]);const auto expected=colors[(i+eye)%colors.size()];
            for(unsigned c=0;c<3;++c)require(std::abs(actual[c]-expected[c])<.06,"asymmetric_marker_or_eye_order_error");
            ++marker_checks;}
        for(unsigned i=1;i<8;++i){const double q=i/8.0;const auto vertical=pixel_at(pixels,width,height,eye,q,.41),horizontal=pixel_at(pixels,width,height,eye,.43,q);
            for(unsigned c=0;c<3;++c)require(vertical[c]<.09&&horizontal[c]<.09,"grid_line_not_at_original_coordinate");
            const auto away_v=pixel_at(pixels,width,height,eye,q+.025,.41),away_h=pixel_at(pixels,width,height,eye,.43,q+.025);
            require(away_v[0]>.2&&away_h[1]>.2,"grid_line_stretched_or_displaced");grid_checks+=2;
        }
    }
    double mean=0,maximum=0;for(auto& eye:eyes)eye.dirty=true;
    constexpr unsigned warmup=5,measured=20;
    for(unsigned i=0;i<warmup+measured;++i){require(renderer.render(eyes),"repeated_render_skipped");const auto ms=renderer.last_gpu_wait_ms();require(std::isfinite(ms)&&ms>=0,"invalid_gpu_time");if(i>=warmup){mean+=ms;maximum=std::max(maximum,ms);}}
    require(vk.read_pixels(width,height)==pixels,"framebuffer_reuse_changed_output");
    // A deferred submission must finish before the next render resets its
    // command buffer, descriptor pool and timestamp slots, even without an
    // explicit complete() call from the caller.
    require(renderer.render(eyes,true),"deferred_reuse_render_skipped");
    require(renderer.last_gpu_wait_ms()==0,"deferred_reuse_waited_on_host");
    require(renderer.render(eyes,true),"next_deferred_render_skipped");
    renderer.complete(false);
    require(vk.read_pixels(width,height)==pixels,"deferred_next_render_changed_pixels");
    const auto gpu_ms=renderer.last_gpu_duration_ms();
    require(gpu_ms==-1||(std::isfinite(gpu_ms)&&gpu_ms>=0),"invalid_gpu_timestamp_span");
    renderer.complete(false);require(renderer.last_gpu_duration_ms()==gpu_ms,"duplicate_complete_changed_timestamp");
    bool direct_upload_checked=false;
    bool packed_sample_checked=false;
    if(const auto allocator=surfxr::vulkan_rgba_frame_allocator(vk.physical,vk.device)){
        const auto fixture=encoded_fixture(sw,sh,true,linear_layout);
        auto frame=std::make_shared<surfxr::VideoFrame>();frame->rgba_storage=allocator(fixture.size());
        require(bool(frame->rgba_storage),"cached_coherent_storage_allocation_failed");
        require(frame->rgba_size()==fixture.size()&&frame->rgba.empty(),"direct_storage_layout_invalid");
        std::memcpy(frame->rgba_data(),fixture.data(),fixture.size());
        const auto* storage=dynamic_cast<const surfxr::VulkanRgbaFrameStorage*>(frame->rgba_storage.get());
        require(storage&&storage->device()==vk.device&&storage->buffer(),"direct_storage_vulkan_identity_invalid");
        // No flush/copy to another mapped allocation: HOST_COHERENT decoder
        // writes are consumed directly by this queued Vulkan transfer.
        vk.upload_buffer(storage->buffer(),sw,sh,false);
        renderer.render(eyes,true);renderer.complete(false);
        require(vk.read_pixels(width,height)==pixels,"direct_mapped_upload_changed_srgb_pixels");
        frame.reset();direct_upload_checked=true;
    }
    if(linear_layout){
        const auto allocator=surfxr::vulkan_rgba_frame_allocator(vk.physical,vk.device);
        auto packed=allocator?allocator(std::size_t{sw}*sh*8):nullptr;
        auto* storage=dynamic_cast<surfxr::VulkanRgbaFrameStorage*>(packed.get());
        if(storage&&storage->prepare_sampled_image(vk.physical,sw*2,sh)){
            const auto fixture=encoded_fixture(sw,sh,true,true);const auto eye_bytes=std::size_t{sw}*sh*4;
            for(unsigned eye=0;eye<2;++eye)for(unsigned row=0;row<sh;++row)
                std::memcpy(packed->data()+(std::size_t{row}*sw*2+eye*sw)*4,
                    fixture.data()+eye*eye_bytes+std::size_t{row}*sw*4,sw*4);
            auto shared=eyes;
            for(unsigned eye=0;eye<2;++eye){auto& input=shared[eye];input.source=storage->sampled_image();input.source_view=storage->sampled_view();
                input.host_source=true;input.host_source_initialized=false;input.source_rect={eye*.5f,0,.5f,1};input.dirty=true;}
            require(renderer.render(shared,true),"packed_source_render_skipped");storage->mark_sampled_image_initialized();renderer.complete(false);
            require(vk.read_pixels(width,height)==pixels,"packed_source_eye_order_or_seam_error");
            for(auto& input:shared)input.host_source_initialized=true;
            // A completed fence permits pooled host rewrites in GENERAL. One
            // clean eye must not trigger a second transition of the shared image.
            shared[0].dirty=false;require(renderer.render(shared,true),"packed_single_eye_render_skipped");renderer.complete(false);
            require(vk.read_pixels(width,height)==pixels,"packed_single_eye_changed_pixels");packed_sample_checked=true;
        }
    }
    std::cout<<"{\"status\":\"gpu_test_passed\",\"source_width\":"<<sw<<",\"source_height\":"<<sh<<",\"width\":"<<width<<",\"height\":"<<height
        <<",\"coordinate_checks\":"<<coordinate_checks<<",\"coordinate_max_linear_error\":"<<max_error<<",\"border_expected_min_original_uv\":["<<min_u<<','<<min_v<<"],\"border_max_linear_error_to_ideal_edge\":"<<border_error<<",\"asymmetric_markers\":"<<marker_checks<<",\"grid_lines\":"<<grid_checks
        <<",\"clean_and_single_eye_cache_checks\":true,\"srgb_checked\":true,\"gpu_wait_ms_mean\":"<<mean/measured<<",\"gpu_wait_ms_max\":"<<maximum
        <<",\"deferred_next_render_checked\":true,\"gpu_timestamp_ms\":"<<gpu_ms
        <<",\"cached_coherent_direct_upload_checked\":"<<(direct_upload_checked?"true":"false")
        <<",\"packed_sample_checked\":"<<(packed_sample_checked?"true":"false")
        <<",\"linear_layout\":"<<(linear_layout?"true":"false")
        <<",\"network_used\":false,\"openxr_session_started\":false,\"hardware_decoder_used\":false}\n";
}
}
