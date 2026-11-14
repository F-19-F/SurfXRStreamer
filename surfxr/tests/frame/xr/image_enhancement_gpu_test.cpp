#include "vulkan_fixture.hpp"
#include "surfxr/image_enhancement_renderer.hpp"
#include "frame/xr/generated/image_enhancement_reference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace surfxr::gpu_test;

enum class RawViews {None,Source,Target,Both};
void run_case(unsigned sw,unsigned sh,unsigned tw,unsigned th,surfxr::ImageEnhancementOptions options,bool constant,unsigned iterations,RawViews raw=RawViews::Both){
    Vulkan vk;vk.initialize();
    for(auto& image:vk.sources)vk.image(image,sw,sh,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,tw,th,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const std::size_t source_bytes=std::size_t(sw)*sh*4,eye_bytes=std::size_t(tw)*th*4;
    vk.buffer(vk.upload,source_bytes*2,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,eye_bytes*2,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::array<surfxr::enhancement_reference::Image,2> sources;
    for(unsigned eye=0;eye<2;++eye){sources[eye]=surfxr::enhancement_reference::fixture(sw,sh,eye);
        if(constant)for(std::size_t i=0;i<source_bytes;i+=4){sources[eye].rgba[i]=eye?212:25;sources[eye].rgba[i+1]=128;sources[eye].rgba[i+2]=eye?38:220;}}
    void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.upload.memory,0,source_bytes*2,0,&mapped),"map_upload");
    for(unsigned eye=0;eye<2;++eye)std::memcpy(static_cast<std::uint8_t*>(mapped)+eye*source_bytes,sources[eye].rgba.data(),source_bytes);
    vkUnmapMemory(vk.device,vk.upload.memory);
    vk.begin();
    for(unsigned eye=0;eye<2;++eye){
        vk.barrier(vk.sources[eye].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};region.bufferOffset=eye*source_bytes;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={sw,sh,1};
        vkCmdCopyBufferToImage(vk.command,vk.upload.buffer,vk.sources[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        vk.barrier(vk.sources[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vk.barrier(vk.targets[eye].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    vk.submit();
    surfxr::ImageEnhancementRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,kFormat,sw,sh,tw,th);
    std::array<surfxr::ImageEnhancementEye,2> eyes{};
    for(unsigned eye=0;eye<2;++eye)eyes[eye]={vk.sources[eye].image,vk.sources[eye].view,vk.targets[eye].view,true};
    require(!renderer.render(eyes,{false,0}),"disabled_must_not_submit_or_touch_target");
    require(renderer.render(eyes,options),"legacy_reference_did_not_submit");
    const auto legacy=vk.read_pixels(tw,th);
    const bool raw_supported=renderer.supports_raw_views();
    const bool raw_source=raw_supported&&(raw==RawViews::Source||raw==RawViews::Both);
    const bool raw_target=raw_supported&&(raw==RawViews::Target||raw==RawViews::Both);
    for(unsigned eye=0;eye<2;++eye){
        eyes[eye].raw_source_view=raw_source?vk.sources[eye].raw_view:VK_NULL_HANDLE;
        eyes[eye].raw_target=raw_target?vk.targets[eye].raw_view:VK_NULL_HANDLE;
    }
    double wait_sum=0,wait_max=0,gpu_sum=0,gpu_max=0;unsigned gpu_samples=0;
    for(unsigned i=0;i<iterations+1;++i){
        require(renderer.render(eyes,options,true),"enabled_effect_did_not_submit");
        require(renderer.last_gpu_wait_ms()==0,"deferred_render_waited_on_host");
        renderer.complete(false);
        const double elapsed=renderer.last_gpu_wait_ms();require(std::isfinite(elapsed)&&elapsed>=0,"invalid_wait_time");
        if(i){wait_sum+=elapsed;wait_max=std::max(wait_max,elapsed);}
        const double gpu=renderer.last_gpu_duration_ms();
        require(gpu==-1||(std::isfinite(gpu)&&gpu>=0),"invalid_gpu_timestamp_span");
        if(i&&gpu>=0){gpu_sum+=gpu;gpu_max=std::max(gpu_max,gpu);++gpu_samples;}
    }
    const auto pixels=vk.read_pixels(tw,th);
    // A same-queue readback covers the deferred render fence. Completion must
    // neither wait again nor lose timestamp results; the next render must also
    // retire pending commands before resetting descriptors/command buffers.
    require(renderer.render(eyes,options,true),"covered_render_did_not_submit");
    require(vk.read_pixels(tw,th)==pixels,"deferred_render_changed_pixels");
    renderer.complete(true);require(renderer.last_gpu_wait_ms()==0,"covered_render_waited_again");
    const auto covered_gpu=renderer.last_gpu_duration_ms();renderer.complete(false);
    require(renderer.last_gpu_duration_ms()==covered_gpu,"duplicate_complete_changed_timestamp");
    require(renderer.render(eyes,options,true)&&renderer.render(eyes,options,true),"pending_next_render_did_not_submit");
    require(vk.read_pixels(tw,th)==pixels,"pending_command_reuse_changed_pixels");renderer.complete(true);
    eyes[0].dirty=eyes[1].dirty=false;require(!renderer.render(eyes,options),"unchanged_pair_must_not_resubmit");
    require(vk.read_pixels(tw,th)==pixels,"clean_pair_changed_output");
    for(std::size_t i=3;i<pixels.size();i+=4)require(pixels[i]==255,"opaque_stereo_alpha");
    const bool full_compare=std::uint64_t(tw)*th<=65536;
    std::vector<std::array<unsigned,2>> probes;
    if(full_compare){for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x)probes.push_back({x,y});}
    else{for(unsigned y=0;y<=24;++y)for(unsigned x=0;x<=32;++x)probes.push_back({(tw-1)*x/32,(th-1)*y/24});}
    double error_sum=0,legacy_error_sum=0;unsigned error_max=0,legacy_error_max=0,changed=0;
    for(unsigned eye=0;eye<2;++eye)for(const auto& probe:probes){
        const auto x=probe[0],y=probe[1];
        const float u=(x+.5f)/tw,v=(y+.5f)/th;
        auto effective=options;effective.super_resolution&=tw>sw||th>sh;
        const auto expected=surfxr::enhancement_reference::enhance(sources[eye],u,v,effective);
        const auto plain=surfxr::enhancement_reference::sample(sources[eye],u,v);
        const auto offset=eye*eye_bytes+(std::size_t(y)*tw+x)*4;
        require(pixels[offset+3]==255,"opaque_stereo_alpha");
        for(unsigned c=0;c<3;++c){
            const int golden=int(std::round(expected[c]*255));const unsigned error=unsigned(std::abs(int(pixels[offset+c])-golden));
            error_sum+=error;error_max=std::max(error_max,error);
            const unsigned legacy_error=unsigned(std::abs(int(pixels[offset+c])-int(legacy[offset+c])));
            legacy_error_sum+=legacy_error;legacy_error_max=std::max(legacy_error_max,legacy_error);
        }
        changed+=std::abs(int(pixels[offset+1])-int(std::round(plain[1]*255)))>=2;
    }
    const double mean=error_sum/(probes.size()*6.0),legacy_mean=legacy_error_sum/(probes.size()*6.0);
    std::cout<<"{\"enhancement_gpu_case\":true,\"source_width\":"<<sw<<",\"source_height\":"<<sh
             <<",\"output_width\":"<<tw<<",\"output_height\":"<<th<<",\"sr\":"<<options.super_resolution
             <<",\"sharpness\":"<<options.sharpness<<",\"constant\":"<<constant<<",\"mean_error\":"<<mean
             <<",\"max_error\":"<<error_max<<",\"changed_pixels\":"<<changed
             <<",\"reference_pixels_checked\":"<<probes.size()*2<<",\"full_reference_compare\":"<<(full_compare?"true":"false")
             <<",\"raw_supported\":"<<(raw_supported?"true":"false")<<",\"raw_source\":"<<(raw_source?"true":"false")<<",\"raw_target\":"<<(raw_target?"true":"false")
             <<",\"legacy_mean_error\":"<<legacy_mean<<",\"legacy_max_error\":"<<legacy_error_max
             <<",\"deferred_completion_checked\":true,\"gpu_samples\":"<<gpu_samples
             <<",\"gpu_mean_ms\":"<<(gpu_samples?gpu_sum/gpu_samples:-1)<<",\"gpu_max_ms\":"<<(gpu_samples?gpu_max:-1)
             <<",\"wait_mean_ms\":"<<wait_sum/iterations<<",\"wait_max_ms\":"<<wait_max<<"}\n";
    require(mean<=1.0&&error_max<=8,"gpu_reference_color_or_sampling_mismatch");
    require(legacy_mean<=1.0&&legacy_error_max<=8,"raw_view_changed_legacy_color_or_sampling");
    require(!full_compare||constant||changed>50,"spatial_effect_is_a_no_op");
    require(!constant||error_max<=1,"constant_color_not_preserved");
}
}
int main(int argc,char** argv){
    try{
        if(argc==2&&std::string(argv[1])=="--self-test"){
            run_case(32,24,48,36,{true,0},true,1);
            run_case(32,24,32,24,{false,1},true,1);
            run_case(32,24,48,36,{true,0},false,1);
            run_case(32,24,48,36,{true,.65f},false,1);
            run_case(32,24,32,24,{false,.65f},false,1);
            run_case(32,24,48,36,{true,.65f},false,1,RawViews::Source);
            run_case(32,24,32,24,{false,.65f},false,1,RawViews::Target);
            run_case(32,24,48,36,{true,.65f},false,1,RawViews::None);
            std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":8,\"gpu_used\":true,\"network_used\":false,\"openxr_session_started\":false}\n";
        }else if((argc>=7&&argc<=9)&&std::string(argv[1])=="--benchmark"){
            const unsigned sw=std::stoul(argv[2]),sh=std::stoul(argv[3]),tw=std::stoul(argv[4]),th=std::stoul(argv[5]),frames=std::stoul(argv[6]);
            require(sw&&sh&&tw&&th&&sw<=8192&&sh<=8192&&tw<=8192&&th<=8192&&frames&&frames<=200,"invalid_benchmark_dimensions");
            const std::string mode=argc>=8?argv[7]:"both";
            require(mode=="sr"||mode=="sharp"||mode=="both","invalid_benchmark_mode");
            require(mode!="sharp"||(sw==tw&&sh==th),"sharp_benchmark_requires_unchanged_extent");
            const std::string views=argc==9?argv[8]:"raw";
            require(views=="raw"||views=="srgb"||views=="raw-source"||views=="raw-target"||views=="compare","invalid_benchmark_views");
            const surfxr::ImageEnhancementOptions options{mode!="sharp",mode=="sr"?0.f:.35f};
            if(views=="compare"){
                run_case(sw,sh,tw,th,options,false,frames,RawViews::None);
                run_case(sw,sh,tw,th,options,false,frames,RawViews::Both);
            }else run_case(sw,sh,tw,th,options,false,frames,
                          views=="srgb"?RawViews::None:views=="raw-source"?RawViews::Source:views=="raw-target"?RawViews::Target:RawViews::Both);
        }else throw std::runtime_error("Usage: surfxr-image-enhancement-gpu-test --self-test | --benchmark sourceW sourceH outputW outputH frames [sr|sharp|both] [raw|srgb|raw-source|raw-target|compare]");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
