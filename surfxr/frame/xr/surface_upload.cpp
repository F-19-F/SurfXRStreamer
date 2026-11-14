#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::create_swapchain(Surface& s,bool request_raw_views) {
    s.raw_requested=request_raw_views;
    std::uint32_t count = 0;
    auto info = xr_info<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format_;
    info.sampleCount = 1;
    info.width = s.width;
    info.height = s.height;
    info.faceCount = info.arraySize = info.mipCount = 1;
    const auto raw_format=format_==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_B8G8R8A8_UNORM;
    if(request_raw_views){
        VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical_,raw_format,&properties);
        constexpr auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        request_raw_views=(properties.optimalTilingFeatures&required)==required;
    }
    // The runtime still composes sRGB storage. Compatible UNORM views let
    // code-domain point/neighborhood operations avoid redundant transfers.
    // https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-swapchain-flags.html
    if(request_raw_views)info.usageFlags|=XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    auto created=xrCreateSwapchain(session_, &info, &s.swapchain);
    if(request_raw_views&&(created==XR_ERROR_FEATURE_UNSUPPORTED||created==XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED)){
        request_raw_views=false;info.usageFlags&=~XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        s.swapchain=XR_NULL_HANDLE;
        created=xrCreateSwapchain(session_, &info, &s.swapchain);
    }
    xr_check(created, "xrCreateSwapchain");
    xr_check(xrEnumerateSwapchainImages(s.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages(count)");
    if (!count || count > 64) throw std::runtime_error("Invalid swapchain image count");
    s.images.assign(count, xr_info<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
    xr_check(xrEnumerateSwapchainImages(s.swapchain, count, &count,
             reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data())), "xrEnumerateSwapchainImages");
    s.images.resize(count);
    s.image_serials.assign(count, 0);
    for(const auto& image:s.images){
        auto vi=vk_info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        vi.image=image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format_;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view=VK_NULL_HANDLE;
        vk_check(vkCreateImageView(device_,&vi,nullptr,&view),"vkCreateImageView(surface)");s.views.push_back(view);
        if(request_raw_views){
            vi.format=raw_format;vi.pNext=nullptr;view=VK_NULL_HANDLE;
            vk_check(vkCreateImageView(device_,&vi,nullptr,&view),"vkCreateImageView(raw surface)");s.raw_views.push_back(view);
        }
    }
}

void XrDisplay::Impl::create_transfer(Surface& s,std::size_t requested_bytes) {
    const auto bytes=requested_bytes?requested_bytes:std::size_t{s.width}*s.height*4;
    if(s.buffer&&s.buffer_bytes>=bytes)return;
    // stage() has retired the previous upload before reaching this point.
    // A mapped desktop allocation may fall back to a full staging buffer
    // on a later frame, so grow the edge-only buffer when necessary.
    if(s.mapped)vkUnmapMemory(device_,s.memory);
    if(s.buffer)vkDestroyBuffer(device_,s.buffer,nullptr);
    if(s.memory)vkFreeMemory(device_,s.memory,nullptr);
    s.mapped=nullptr;s.buffer=VK_NULL_HANDLE;s.memory=VK_NULL_HANDLE;s.buffer_bytes=0;
    auto buffer = vk_info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(device_, &buffer, nullptr, &s.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, s.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    std::uint32_t index = UINT32_MAX;

    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1U << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            // Keep the measured coherent write-combined upload path. CPU
            // edge/channel operations now only read the normal source RAM.
            index=i;
            if(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
        }
    }
    if (index == UINT32_MAX) throw std::runtime_error("No host-visible Vulkan staging memory");
    const auto selected=memory.memoryTypes[index].propertyFlags;
    s.coherent=(selected&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    s.cached=(selected&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=0;
    auto allocation = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = index;
    vk_check(vkAllocateMemory(device_, &allocation, nullptr, &s.memory), "vkAllocateMemory");
    vk_check(vkBindBufferMemory(device_, s.buffer, s.memory, 0), "vkBindBufferMemory");
    vk_check(vkMapMemory(device_, s.memory, 0, VK_WHOLE_SIZE, 0, &s.mapped), "vkMapMemory");
    s.buffer_bytes=bytes;
}

void XrDisplay::Impl::create_commands() {
    auto pool = vk_info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    vk_check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    auto commands = vk_info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    commands.commandPool = pool_;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_, &commands, &command_), "vkAllocateCommandBuffers");
    auto fence = vk_info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    vk_check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
}

void XrDisplay::Impl::record_copy(Surface& s,std::uint32_t index) {
    if(index>=s.images.size())throw std::runtime_error("Invalid swapchain image index");
    // OpenXR supplies and expects COLOR_ATTACHMENT_OPTIMAL on this queue.
    auto barrier = vk_info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = s.images[index].image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {s.width, s.height, 1};
    if(s.edge_plan.valid()){
        if(!s.direct_buffer||!s.buffer||!s.direct_frame)throw std::runtime_error("Desktop direct upload ownership missing");
        region.bufferOffset=s.direct_offset;region.bufferRowLength=s.direct_row_length;
        region.imageOffset={2,2,0};region.imageExtent={s.width-4,s.height-4,1};
        vkCmdCopyBufferToImage(command_,s.direct_buffer,s.images[index].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        std::array<VkBufferImageCopy,4> edges{};
        for(unsigned i=0;i<edges.size();++i){const auto& part=s.edge_plan.regions[i];auto& edge=edges[i];
            edge.bufferOffset=part.offset;edge.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            edge.imageOffset={static_cast<std::int32_t>(part.x),static_cast<std::int32_t>(part.y),0};
            edge.imageExtent={part.width,part.height,1};
        }
        // Interior/strips are disjoint. No pixel is overwritten twice and
        // no extra transfer-to-transfer dependency is necessary.
        vkCmdCopyBufferToImage(command_,s.buffer,s.images[index].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(edges.size()),edges.data());
    }else{
        if(s.direct_buffer){region.bufferOffset=s.direct_offset;region.bufferRowLength=s.direct_row_length;}
        vkCmdCopyBufferToImage(command_, s.direct_buffer?s.direct_buffer:s.buffer, s.images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool XrDisplay::Impl::needs_upload(const Acquired& a) {
    return !a.suppress_upload&&(a.surface->buffer||a.surface->direct_buffer||a.surface->nv12_storage)&&a.surface->image_serials[a.index]!=a.surface->serial;
}

void XrDisplay::Impl::complete_copy_batch(const std::vector<Acquired>& acquired,bool covered_by_render) {
    if(!copy_pending_)return;
    if(covered_by_render){
        // The successful fence wait of a later vkQueueSubmit on queue_
        // covers all earlier commands and fence signals on that queue.
        // Both renderers' transfer->shader barriers provide the memory
        // dependency needed to sample the uploaded source images.
        // https://docs.vulkan.org/spec/latest/chapters/synchronization.html
        ++stats_.deferred_copy_covered_completions;
    }else{
        const auto before=Clock::now();const auto result=vkWaitForFences(device_,1,&fence_,VK_TRUE,2'000'000'000ULL);
        const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count();
        ++stats_.gpu_wait_calls;stats_.gpu_wait_ms_sum+=ms;stats_.gpu_wait_ms_max=std::max(stats_.gpu_wait_ms_max,ms);
        vk_check(result,"vkWaitForFences(batch,2s)");
    }
    for(const auto& a:acquired)if(!a.surface->nv12_storage&&needs_upload(a)){
        a.surface->image_serials[a.index]=a.surface->serial;++stats_.upload_count;
        for(unsigned i=0;i<sources_.size();++i)if(a.surface==&surfaces_[i]){++stats_.desktop_uploads;break;}
    }
    copy_pending_=false;
}

void XrDisplay::Impl::copy_batch(const std::vector<Acquired>& acquired,bool defer_completion) {
    if(copy_pending_)throw std::runtime_error("Upload batch still pending");
    // GPU-only reconstruction targets have no CPU transfer buffer.
    bool dirty=false;for(const auto& a:acquired)dirty|=!a.surface->nv12_storage&&needs_upload(a);
    if(!dirty)return;
    vk_check(vkResetCommandBuffer(command_,0),"vkResetCommandBuffer");
    vk_check(vkResetFences(device_,1,&fence_),"vkResetFences");
    auto begin=vk_info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command_,&begin),"vkBeginCommandBuffer");
    for(const auto& a:acquired)if(!a.surface->nv12_storage&&needs_upload(a))record_copy(*a.surface,a.index);
    vk_check(vkEndCommandBuffer(command_),"vkEndCommandBuffer");
    auto submit=vk_info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);submit.commandBufferCount=1;submit.pCommandBuffers=&command_;
    vk_check(vkQueueSubmit(queue_,1,&submit,fence_),"vkQueueSubmit(batch)");
    copy_pending_=true;
    if(defer_completion)++stats_.deferred_copy_batches;
    else complete_copy_batch(acquired,false);
}

} // namespace surfxr
