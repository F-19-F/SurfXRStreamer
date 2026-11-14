#include "projection_renderer_test_cases.hpp"
#include "surfxr/foveation_renderer.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"

namespace surfxr::tests::projection {
namespace {
std::vector<std::uint8_t> read_eyes(Vulkan& vk,unsigned target){
    const std::size_t bytes=std::size_t{target}*target*4;
    vk.begin();
    for(unsigned eye=0;eye<2;++eye){
        vk.barrier(vk.targets[eye].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.bufferOffset=eye*bytes;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={target,target,1};
        vkCmdCopyImageToBuffer(vk.command,vk.targets[eye].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,vk.readback.buffer,1,&copy);
    }
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=vk.readback.buffer;host.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(vk.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);vk.submit();
    std::vector<std::uint8_t> pixels(bytes*2);void* mapped=nullptr;
    checked(vkMapMemory(vk.device,vk.readback.memory,0,pixels.size(),0,&mapped),"NV12_projection_readback");
    std::memcpy(pixels.data(),mapped,pixels.size());vkUnmapMemory(vk.device,vk.readback.memory);return pixels;
}
void fill_nv12(VulkanNv12FrameStorage& storage,unsigned pattern){
    for(unsigned y=0;y<storage.height;++y)for(unsigned x=0;x<storage.width;++x){
        // Text strokes, single-pixel checkerboards, endpoints and smooth ramps.
        storage.data(0)[y*storage.stride[0]+x]=static_cast<std::uint8_t>(pattern%2?
            ((x%17<2||y%23<2)?16:235):((x^y)&1?x*255/(storage.width-1):y*255/(storage.height-1)));
    }
    for(unsigned y=0;y<storage.height/2;++y)for(unsigned x=0;x<storage.width/2;++x){
        auto* p=storage.data(1)+y*storage.stride[1]+x*2;
        p[0]=static_cast<std::uint8_t>((x/13*37+pattern*41)%256);p[1]=static_cast<std::uint8_t>((y/11*53+pattern*29)%256);
    }
}
}
void gpu_nv12_test(){
    constexpr unsigned target=512;
    unsigned cases=0,max_difference=0;std::uint64_t channels=0,changed=0;
    for(unsigned quality=0;quality<2;++quality)for(unsigned raw=0;raw<2;++raw)for(unsigned odd=0;odd<2;++odd){
        const auto sampling=quality?ProjectionSampling::QualitySS:ProjectionSampling::Baseline;
        const auto other=quality?ProjectionSampling::Baseline:ProjectionSampling::QualitySS;
        Vulkan vk;vk.initialize(true);const unsigned width=kSourceWidth-odd,height=kSourceHeight-odd;
        vk.image(vk.source,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        for(auto& image:vk.targets)vk.image(image,target,target,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        vk.buffer(vk.readback,std::size_t{target}*target*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        FoveationRenderer conversion(vk.physical,vk.device,vk.queue,vk.family,kFormat,width,height);
        ProjectionRenderer reference(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,1);
        ProjectionRenderer fused(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,1,0,raw!=0);
        std::array<ProjectionEye,2> eyes{};
        for(unsigned eye=0;eye<2;++eye)eyes[eye]={vk.targets[eye].view,{{eye?.032f:-.032f,0,0},{}},{-.65f,.65f,.65f,-.65f}};
        for(unsigned matrix=0;matrix<3;++matrix)for(unsigned limited=0;limited<2;++limited)for(unsigned footprint=0;footprint<3;++footprint){
            auto storage_base=VulkanNv12FrameStorage::allocate(vk.physical,vk.device,kSourceWidth,kSourceHeight);
            auto* storage=dynamic_cast<VulkanNv12FrameStorage*>(storage_base.get());require(storage,"allocate_NV12_projection_fixture");
            fill_nv12(*storage,footprint+matrix);
            const std::array<float,2> coefficients[3]={{.299f,.114f},{.2126f,.0722f},{.2627f,.0593f}};
            ProjectionSource source{};source.image=storage->image(0);source.view=storage->view(0);
            source.chroma_image=storage->image(1);source.chroma_view=storage->view(1);
            source.width=width;source.height=height;source.content_generation=cases+1;
            source.source_rect=odd?std::array<float,4>{.0625f,.125f,.875f,.75f}:std::array<float,4>{0,0,1,1};
            source.yuv_coefficients={coefficients[matrix][0],coefficients[matrix][1],limited?16.f/255:0.f,limited?255.f/219:1.f};
            source.panel.pose={{.031f,-.023f,-2},{0,std::sin(.08f),0,std::cos(.08f)}};
            source.panel.width=footprint==0?2.8f:footprint==1?.8f:.2f;source.panel.height=source.panel.width*height/width;
            source.panel.curvature_radians=footprint==1?.8f:0.f;
            // Exercise PREINITIALIZED -> GENERAL in the new production path first.
            fused.render({source},eyes,sampling);storage->mark_initialized();source.nv12_initialized=true;
            const auto actual=read_eyes(vk,target);
            require(fused.can_reuse_source(0,width,height,source.content_generation,sampling)&&
                !fused.can_reuse_source(0,width,height,source.content_generation,other),"NV12_cache_sampling_key_missing");
            fused.render({source},eyes,sampling);
            require(read_eyes(vk,target)==actual,"NV12_cached_pixels_changed");
            std::array<FoveationEye,2> conversion_eyes{};conversion_eyes[1].dirty=false;
            auto& convert=conversion_eyes[0];convert.source=storage->image(0);convert.source_view=storage->view(0);convert.target=vk.source.view;
            convert.host_source=true;convert.host_source_initialized=true;convert.linear_layout=true;convert.desktop_border=true;
            convert.chroma_source=storage->image(1);convert.chroma_view=storage->view(1);convert.source_rect=source.source_rect;convert.yuv_coefficients=source.yuv_coefficients;
            conversion.render(conversion_eyes);
            auto rgba=source;rgba.image=vk.source.image;rgba.view=vk.source.view;rgba.chroma_image=VK_NULL_HANDLE;rgba.chroma_view=VK_NULL_HANDLE;
            reference.render({rgba},eyes,sampling);const auto expected=read_eyes(vk,target);
            unsigned visible=0;
            for(std::size_t i=0;i<actual.size();++i){const auto difference=static_cast<unsigned>(std::abs(int(actual[i])-int(expected[i])));
                max_difference=std::max(max_difference,difference);changed+=difference!=0;++channels;if(i%4==3&&actual[i])++visible;}
            require(visible>200,"NV12_projection_fixture_empty");
            require(max_difference<=2,"NV12_fused_projection_exceeds_two_code_values");
            const auto a=fused.statistics(),b=reference.statistics();
            if(quality){
                require(a.cached_bytes==b.cached_bytes&&a.cached_levels==b.cached_levels,"NV12_mip_chain_changed");
                require(a.nv12_mip_updates==b.mip_updates*2-1&&a.mip_reuses==b.mip_updates,"NV12_cache_generation_not_reused");
            }else{
                require(a.cached_levels==1&&a.cached_bytes==std::uint64_t{width}*height*4&&a.mip_updates==0,"baseline_generated_mips");
                require(a.nv12_base_updates==a.baseline_frames-a.nv12_base_reuses&&a.nv12_base_reuses*2==a.nv12_base_updates+1,"baseline_cache_generation_not_reused");
            }
            fused.invalidate(0);fused.render({source},eyes,sampling);
            require(read_eyes(vk,target)==actual,"NV12_invalidated_pixels_changed");
            ++cases;
        }
        require(fused.statistics().nv12_intermediate_bytes_avoided==std::uint64_t{width}*height*4*36,"NV12_avoided_payload_count_incorrect");
        // Switch modes with the exact same NV12 generation, including a 1x1
        // cache where the mip count alone cannot identify premultiplication.
        auto storage_base=VulkanNv12FrameStorage::allocate(vk.physical,vk.device,2,2);
        auto* storage=dynamic_cast<VulkanNv12FrameStorage*>(storage_base.get());fill_nv12(*storage,1);
        ProjectionSource tiny{};tiny.image=storage->image(0);tiny.view=storage->view(0);tiny.chroma_image=storage->image(1);tiny.chroma_view=storage->view(1);
        tiny.width=tiny.height=1;tiny.content_generation=cases+1;tiny.yuv_coefficients={.2126f,.0722f,0,1};
        tiny.panel.pose.position={0,0,-2};tiny.panel.width=tiny.panel.height=1;
        fused.render({tiny},eyes,sampling);storage->mark_initialized();tiny.nv12_initialized=true;
        const auto before_switch=read_eyes(vk,target);
        fused.render({tiny},eyes,other);
        require(!fused.can_reuse_source(0,1,1,tiny.content_generation,sampling),"NV12_mode_switch_reused_wrong_alpha");
        fused.render({tiny},eyes,sampling);require(read_eyes(vk,target)==before_switch,"NV12_mode_switch_changed_pixels");
    }
    std::cout<<"{\"status\":\"gpu_nv12_projection_passed\",\"cases\":"<<cases<<",\"channels_compared\":"<<channels
        <<",\"changed_channels\":"<<changed<<",\"max_code_difference\":"<<max_difference
        <<",\"network_used\":false,\"openxr_session_started\":false}\n";
}
}
