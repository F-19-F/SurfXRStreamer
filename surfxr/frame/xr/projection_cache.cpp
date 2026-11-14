#include "projection_private.hpp"

namespace surfxr {
void ProjectionRenderer::Impl::release_cache(MipCache& cache){
        if(cache.framebuffer)vkDestroyFramebuffer(device,cache.framebuffer,nullptr);
        if(cache.raw_framebuffer)vkDestroyFramebuffer(device,cache.raw_framebuffer,nullptr);
        if(cache.base_view)vkDestroyImageView(device,cache.base_view,nullptr);
        if(cache.raw_view)vkDestroyImageView(device,cache.raw_view,nullptr);
        if(cache.image.view)vkDestroyImageView(device,cache.image.view,nullptr);
        if(cache.image.image)vkDestroyImage(device,cache.image.image,nullptr);
        if(cache.image.memory)vkFreeMemory(device,cache.image.memory,nullptr);
        cache={};
    }

void ProjectionRenderer::Impl::prepare_cache(MipCache& cache,const ProjectionSource& source,bool quality){
        const bool raw=image_format_list&&source.chroma_view;
        if(cache.image.image&&cache.width==source.width&&cache.height==source.height&&cache.raw==raw&&cache.quality==quality)return;
        // The preceding render is fenced; source image/view identity is not a
        // cache key because XR rotates them even when decoded pixels repeat.
        release_cache(cache);
        cache.raw=raw;cache.quality=quality;cache.width=source.width;cache.height=source.height;
        cache.levels=quality?mip_levels(source.width,source.height):1;
        try{
            auto ci=info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;
            // Compatible sRGB/UNORM views share the same mip storage. NV12's
            // opaque encoded pixels need no EOTF/OETF before this first store.
            const std::array<VkFormat,2> formats{format,raw_format};
            auto list=info<VkImageFormatListCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
            list.viewFormatCount=static_cast<unsigned>(formats.size());list.pViewFormats=formats.data();
            if(raw){ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;ci.pNext=&list;}
            ci.extent={source.width,source.height,1};ci.mipLevels=cache.levels;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
            ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if(quality)ci.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            check(vkCreateImage(device,&ci,nullptr,&cache.image.image),"vkCreateImage(quality mips)");
            VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,cache.image.image,&req);
            auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;
            ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device,&ai,nullptr,&cache.image.memory),"vkAllocateMemory(quality mips)");
            check(vkBindImageMemory(device,cache.image.image,cache.image.memory,0),"vkBindImageMemory(quality mips)");
            auto vi=info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);vi.image=cache.image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,cache.levels,0,1};
            check(vkCreateImageView(device,&vi,nullptr,&cache.image.view),"vkCreateImageView(quality mips)");
            vi.subresourceRange.levelCount=1;check(vkCreateImageView(device,&vi,nullptr,&cache.base_view),"vkCreateImageView(quality mip0)");
            auto fb=info<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);fb.renderPass=mip_pass;fb.attachmentCount=1;
            fb.pAttachments=&cache.base_view;fb.width=cache.width;fb.height=cache.height;fb.layers=1;
            check(vkCreateFramebuffer(device,&fb,nullptr,&cache.framebuffer),"vkCreateFramebuffer(quality mip0)");
            if(raw){vi.format=raw_format;check(vkCreateImageView(device,&vi,nullptr,&cache.raw_view),"vkCreateImageView(NV12 mip0)");
            fb.renderPass=nv12_mip_pass;fb.pAttachments=&cache.raw_view;
            check(vkCreateFramebuffer(device,&fb,nullptr,&cache.raw_framebuffer),"vkCreateFramebuffer(NV12 mip0)");}
        }catch(...){release_cache(cache);throw;}
    }

void ProjectionRenderer::Impl::barrier(VkImage image,unsigned base,unsigned count,VkImageLayout old_layout,VkImageLayout new_layout,
                 VkAccessFlags before,VkAccessFlags after,VkPipelineStageFlags from,VkPipelineStageFlags to){
        auto b=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b.srcAccessMask=before;b.dstAccessMask=after;
        b.oldLayout=old_layout;b.newLayout=new_layout;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,base,count,0,1};
        vkCmdPipelineBarrier(command,from,to,0,0,nullptr,0,nullptr,1,&b);
    }

void ProjectionRenderer::Impl::source_transitions(const std::vector<ProjectionSource>& sources,const std::array<bool,max_sources>& update,bool quality,bool start){
    std::vector<VkImageMemoryBarrier> barriers;
    VkPipelineStageFlags from=0,to=0;
    for(unsigned i=0;i<sources.size();++i){
        if((quality||sources[i].chroma_view)&&!update[i])continue;
        const auto& source=sources[i];const bool nv12=source.chroma_view!=VK_NULL_HANDLE;
        if(nv12&&!start&&!source.nv12_foreign)continue;
        for(unsigned plane=0;plane<(nv12?2u:1u);++plane){
            const auto image=plane?source.chroma_image:source.image;
            if(plane&&image==source.image)continue;
            auto b=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(nv12){
                b.oldLayout=start&&!source.nv12_foreign&&!source.nv12_initialized?VK_IMAGE_LAYOUT_PREINITIALIZED:VK_IMAGE_LAYOUT_GENERAL;
                b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                if(source.nv12_foreign){
                    b.srcQueueFamilyIndex=start?VK_QUEUE_FAMILY_FOREIGN_EXT:family;
                    b.dstQueueFamilyIndex=start?family:VK_QUEUE_FAMILY_FOREIGN_EXT;
                }
                b.srcAccessMask=start?(source.nv12_foreign?0:VK_ACCESS_HOST_WRITE_BIT):VK_ACCESS_SHADER_READ_BIT;
                b.dstAccessMask=start?VK_ACCESS_SHADER_READ_BIT:0;
                from|=start?(source.nv12_foreign?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_HOST_BIT):VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                to|=start?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            }else{
                b.oldLayout=start?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.newLayout=start?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b.srcAccessMask=start?(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT):VK_ACCESS_SHADER_READ_BIT;
                b.dstAccessMask=start?VK_ACCESS_SHADER_READ_BIT:(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                from|=start?(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT):VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                to|=start?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            }
            barriers.push_back(b);
        }
    }
    if(!barriers.empty())vkCmdPipelineBarrier(command,from,to,0,0,nullptr,0,nullptr,static_cast<unsigned>(barriers.size()),barriers.data());
}

void ProjectionRenderer::Impl::update_mips(MipCache& cache,VkDescriptorSet source,const ProjectionSource& input){
        const auto old=cache.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        const VkAccessFlags before=cache.initialized?VkAccessFlags{VK_ACCESS_SHADER_READ_BIT}:VkAccessFlags{0};
        const auto from=cache.initialized?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier(cache.image.image,0,1,old,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,before,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,from,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if(cache.levels>1)barrier(cache.image.image,1,cache.levels-1,old,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,before,VK_ACCESS_TRANSFER_WRITE_BIT,from,VK_PIPELINE_STAGE_TRANSFER_BIT);
        auto begin=info<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        begin.renderPass=cache.raw?nv12_mip_pass:mip_pass;begin.framebuffer=cache.raw?cache.raw_framebuffer:cache.framebuffer;
        begin.renderArea.extent={cache.width,cache.height};
        const auto nv12=cache.quality?(cache.raw?nv12_mip_pipeline:nv12_linear_mip_pipeline):
            (cache.raw?nv12_base_pipeline:nv12_linear_base_pipeline);
        vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,input.chroma_view?nv12:mip_pipeline);
        // The shared vertex module still declares mesh inputs statically even
        // though mode2 uses gl_VertexIndex. Keep their bindings valid too.
        const VkDeviceSize vertex_offset=0;vkCmdBindVertexBuffers(command,0,1,&vertices,&vertex_offset);
        const VkViewport viewport{0,0,static_cast<float>(cache.width),static_cast<float>(cache.height),0,1};const VkRect2D scissor{{0,0},{cache.width,cache.height}};
        vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
        Camera copy{};
        if(input.chroma_view){
            std::copy(input.source_rect.begin(),input.source_rect.end(),copy.vp.begin());
            std::copy(input.yuv_coefficients.begin(),input.yuv_coefficients.end(),copy.vp.begin()+4);
            copy.vp[8]=static_cast<float>(cache.width);copy.vp[9]=static_cast<float>(cache.height);
        }
        vkCmdPushConstants(command,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Camera),&copy);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&source,0,nullptr);vkCmdDraw(command,3,1,0,0);
        vkCmdEndRenderPass(command);
        if(cache.levels==1){barrier(cache.image.image,0,1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);return;}
        barrier(cache.image.image,0,1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        unsigned w=cache.width,h=cache.height;
        for(unsigned level=1;level<cache.levels;++level){
            const unsigned next_w=std::max(1u,w/2),next_h=std::max(1u,h/2);
            VkImageBlit region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level-1,0,1};region.srcOffsets[1]={static_cast<int>(w),static_cast<int>(h),1};
            region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};region.dstOffsets[1]={static_cast<int>(next_w),static_cast<int>(next_h),1};
            // Vulkan sRGB blits decode before linear filtering and encode into
            // the destination. RGB is already premultiplied; alpha is linear.
            vkCmdBlitImage(command,cache.image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,cache.image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_LINEAR);
            // Only the next blit needs this dependency now. Completed levels
            // remain transfer sources until the entire chain is ready.
            if(level+1<cache.levels)barrier(cache.image.image,level,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            w=next_w;h=next_h;
        }
        // One transfer-to-fragment boundary publishes every completed level.
        // Retain write visibility for all blits, including the final level
        // which is still a transfer destination and has never been read.
        std::array<VkImageMemoryBarrier,2> ready{};
        for(auto& b:ready){b=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=cache.image.image;
            b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
        ready[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;ready[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,cache.levels-1,0,1};
        ready[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;ready[1].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,cache.levels-1,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,
            static_cast<unsigned>(ready.size()),ready.data());
    }
}
