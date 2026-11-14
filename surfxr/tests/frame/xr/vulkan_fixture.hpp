#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "surfxr/vulkan_dmabuf.hpp"
namespace surfxr::gpu_test {
constexpr VkFormat kFormat=VK_FORMAT_R8G8B8A8_SRGB;
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
void checked(VkResult value,const char* label){if(value!=VK_SUCCESS)throw std::runtime_error(std::string(label)+"_"+std::to_string(value));}
struct Image {VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE,raw_view=VK_NULL_HANDLE;};
struct Buffer {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;};
struct Vulkan {
    VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;
    unsigned family=0;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    std::array<Image,2> sources{},targets{};Buffer upload,readback;
    ~Vulkan(){
        if(device){vkDeviceWaitIdle(device);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            for(auto* image:{&sources[0],&sources[1],&targets[0],&targets[1]}){if(image->raw_view)vkDestroyImageView(device,image->raw_view,nullptr);if(image->view)vkDestroyImageView(device,image->view,nullptr);if(image->image)vkDestroyImage(device,image->image,nullptr);if(image->memory)vkFreeMemory(device,image->memory,nullptr);}
            for(auto* buffer:{&upload,&readback}){if(buffer->buffer)vkDestroyBuffer(device,buffer->buffer,nullptr);if(buffer->memory)vkFreeMemory(device,buffer->memory,nullptr);}
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
    std::uint32_t memory_type(std::uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        for(std::uint32_t i=0;i<properties.memoryTypeCount;++i)if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("required_memory_type_unavailable");
    }
    void initialize(bool /*reserved*/=false,std::uint32_t api_version=VK_API_VERSION_1_0,bool dmabuf=false){
        VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="surfxr-image-enhancement-offline-test";app.apiVersion=api_version;
        VkInstanceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;info.pApplicationInfo=&app;checked(vkCreateInstance(&info,nullptr,&instance),"vkCreateInstance");
        std::uint32_t count=0;checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"vkEnumeratePhysicalDevices_count");require(count>0,"no_vulkan_device");
        std::vector<VkPhysicalDevice> devices(count);checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"vkEnumeratePhysicalDevices");
        for(auto candidate:devices){std::uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);std::vector<VkQueueFamilyProperties> properties(n);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,properties.data());
            VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(candidate,kFormat,&format);
            const auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if((format.optimalTilingFeatures&required)!=required)continue;
            for(unsigned i=0;i<n;++i)if(properties[i].queueCount&&(properties[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){physical=candidate;family=i;break;}
            if(physical)break;
        }
        require(physical!=VK_NULL_HANDLE,"no_graphics_srgb_device");const float priority=1;
        VkDeviceQueueCreateInfo q{};q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&priority;
        VkDeviceCreateInfo d{};d.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;
        VulkanDmaBufFeatures external;std::vector<const char*> enabled;
        if(dmabuf){require(external.query(physical),"dmabuf_interop_unavailable");external.attach(d,enabled);}
        checked(vkCreateDevice(physical,&d,nullptr,&device),"vkCreateDevice");vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=family;checked(vkCreateCommandPool(device,&p,nullptr,&pool),"vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;alloc.commandPool=pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;checked(vkAllocateCommandBuffers(device,&alloc,&command),"vkAllocateCommandBuffers");
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;checked(vkCreateFence(device,&f,nullptr,&fence),"vkCreateFence");
    }
    void image(Image& out,unsigned width,unsigned height,VkImageUsageFlags usage){
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;i.imageType=VK_IMAGE_TYPE_2D;i.format=kFormat;i.extent={width,height,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        checked(vkCreateImage(device,&i,nullptr,&out.image),"vkCreateImage");VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,out.image,&requirements);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=requirements.size;a.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);checked(vkAllocateMemory(device,&a,nullptr,&out.memory),"vkAllocateMemory_image");checked(vkBindImageMemory(device,out.image,out.memory,0),"vkBindImageMemory");
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=out.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=kFormat;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};checked(vkCreateImageView(device,&view,nullptr,&out.view),"vkCreateImageView");
        view.format=VK_FORMAT_R8G8B8A8_UNORM;checked(vkCreateImageView(device,&view,nullptr,&out.raw_view),"vkCreateImageView_raw");
    }
    void buffer(Buffer& out,VkDeviceSize size,VkBufferUsageFlags usage){
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=size;b.usage=usage;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateBuffer(device,&b,nullptr,&out.buffer),"vkCreateBuffer");VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,out.buffer,&requirements);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=requirements.size;a.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);checked(vkAllocateMemory(device,&a,nullptr,&out.memory),"vkAllocateMemory_buffer");checked(vkBindBufferMemory(device,out.buffer,out.memory,0),"vkBindBufferMemory");
    }
    void begin(){checked(vkResetCommandBuffer(command,0),"vkResetCommandBuffer");VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&b),"vkBeginCommandBuffer");}
    void submit(){checked(vkEndCommandBuffer(command),"vkEndCommandBuffer");checked(vkResetFences(device,1,&fence),"vkResetFences");VkSubmitInfo info{};info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;info.commandBufferCount=1;info.pCommandBuffers=&command;checked(vkQueueSubmit(queue,1,&info,fence),"vkQueueSubmit");checked(vkWaitForFences(device,1,&fence,VK_TRUE,2'000'000'000ULL),"vkWaitForFences");}
    void barrier(VkImage image,VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags source_access,VkAccessFlags target_access,VkPipelineStageFlags source_stage,VkPipelineStageFlags target_stage){
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=source_access;b.dstAccessMask=target_access;b.oldLayout=old_layout;b.newLayout=new_layout;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,source_stage,target_stage,0,0,nullptr,0,nullptr,1,&b);
    }
    std::vector<std::uint8_t> read_pixels(unsigned width,unsigned height){
        const std::size_t eye_bytes=std::size_t(width)*height*4;
        begin();
        for(unsigned eye=0;eye<2;++eye){
            barrier(targets[eye].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};region.bufferOffset=eye*eye_bytes;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={width,height,1};
            vkCmdCopyImageToBuffer(command,targets[eye].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&region);
            barrier(targets[eye].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback.buffer;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);submit();
        std::vector<std::uint8_t> pixels(eye_bytes*2);void* mapped=nullptr;
        checked(vkMapMemory(device,readback.memory,0,pixels.size(),0,&mapped),"map_readback");
        std::memcpy(pixels.data(),mapped,pixels.size());vkUnmapMemory(device,readback.memory);return pixels;
    }
};

}
