#pragma once
#include "surfxr/latest_frame.hpp"
#include <vulkan/vulkan.h>

namespace surfxr {
// A decoder-owned RGBA allocation that is also a GPU transfer source. The
// caller must retain the complete immutable VideoFrame through its last GPU
// read (upload or sampled reconstruction), since the pool checks frame ownership,
// destroy every allocation and allocator callback before destroying VkDevice.
class VulkanRgbaFrameStorage final : public RgbaFrameStorage {
public:
    VulkanRgbaFrameStorage(VkDevice device,std::size_t bytes):device_(device),bytes_(bytes){}
    ~VulkanRgbaFrameStorage() override {
        if(sampled_view_)vkDestroyImageView(device_,sampled_view_,nullptr);
        if(sampled_image_)vkDestroyImage(device_,sampled_image_,nullptr);
        if(mapped_)vkUnmapMemory(device_,memory_);
        if(buffer_)vkDestroyBuffer(device_,buffer_,nullptr);
        if(memory_)vkFreeMemory(device_,memory_,nullptr);
    }
    VulkanRgbaFrameStorage(const VulkanRgbaFrameStorage&)=delete;
    VulkanRgbaFrameStorage& operator=(const VulkanRgbaFrameStorage&)=delete;
    std::uint8_t* data() noexcept override{return static_cast<std::uint8_t*>(mapped_);}
    std::size_t size() const noexcept override{return bytes_;}
    VkDevice device() const noexcept{return device_;}
    VkBuffer buffer() const noexcept{return buffer_;}
    // Called only by the rendering owner on a Vulkan 1.1+ device. The decoder
    // keeps writing the same tight RGBA bytes; a compatible linear sRGB image
    // aliases this non-dedicated allocation without a transfer or repacking.
    // Cache both success and failure for this allocation's original shape.
    bool prepare_sampled_image(VkPhysicalDevice physical,unsigned width,unsigned height) noexcept {
        if(sampled_probe_attempted_)return sampled_image_&&width==sampled_width_&&height==sampled_height_;
        sampled_probe_attempted_=true;sampled_width_=width;sampled_height_=height;
        if(!physical||!device_||!buffer_||!memory_||!mapped_||!width||!height||width>8192||height>8192||
           std::uint64_t{width}*height*4!=bytes_||memory_type_>=32)return false;
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
        if(properties.apiVersion<VK_API_VERSION_1_1)return false;
        const auto buffer_requirements=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(vkGetDeviceProcAddr(device_,"vkGetBufferMemoryRequirements2"));
        const auto image_requirements=reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(vkGetDeviceProcAddr(device_,"vkGetImageMemoryRequirements2"));
        if(!buffer_requirements||!image_requirements)return false;
        VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_SRGB,&features);
        constexpr auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((features.linearTilingFeatures&required)!=required)return false;
        VkImageFormatProperties supported{};
        if(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_R8G8B8A8_SRGB,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,
            VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_CREATE_ALIAS_BIT,&supported)!=VK_SUCCESS||
            width>supported.maxExtent.width||height>supported.maxExtent.height||!(supported.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return false;
        // The original allocation deliberately has no dedicated-allocation
        // pNext. An image or buffer requiring one cannot share it legally.
        VkMemoryDedicatedRequirements dedicated{};dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        VkMemoryRequirements2 requirements{};requirements.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;requirements.pNext=&dedicated;
        VkBufferMemoryRequirementsInfo2 buffer_info{};buffer_info.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;buffer_info.buffer=buffer_;
        buffer_requirements(device_,&buffer_info,&requirements);
        if(dedicated.requiresDedicatedAllocation||requirements.memoryRequirements.size>allocation_bytes_||
           !(requirements.memoryRequirements.memoryTypeBits&(1u<<memory_type_)))return false;
        VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.flags=VK_IMAGE_CREATE_ALIAS_BIT;
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_SRGB;ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_LINEAR;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
        // Pixels may already have been written before lazy image creation.
        // UNDEFINED would permit the first transition to discard those bytes.
        ci.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
        VkImage image=VK_NULL_HANDLE;if(vkCreateImage(device_,&ci,nullptr,&image)!=VK_SUCCESS)return false;
        const auto reject=[&](){vkDestroyImage(device_,image,nullptr);return false;};
        VkImageMemoryRequirementsInfo2 image_info{};image_info.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;image_info.image=image;
        dedicated.requiresDedicatedAllocation=dedicated.prefersDedicatedAllocation=VK_FALSE;
        image_requirements(device_,&image_info,&requirements);const auto& req=requirements.memoryRequirements;
        // Both resources bind at allocation offset zero, satisfying either
        // nonzero alignment; their memory-type intersection and size must fit.
        if(dedicated.requiresDedicatedAllocation||!req.alignment||req.size>allocation_bytes_||!(req.memoryTypeBits&(1u<<memory_type_)))return reject();
        const VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(device_,image,&subresource,&layout);
        if(layout.offset!=0||layout.rowPitch!=std::uint64_t{width}*4||layout.size<bytes_||layout.size>allocation_bytes_)return reject();
        if(vkBindImageMemory(device_,image,memory_,0)!=VK_SUCCESS)return reject();
        VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=VK_FORMAT_R8G8B8A8_SRGB;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view=VK_NULL_HANDLE;
        if(vkCreateImageView(device_,&vi,nullptr,&view)!=VK_SUCCESS)return reject();
        sampled_image_=image;sampled_view_=view;return true;
    }
    VkImage sampled_image() const noexcept{return sampled_image_;}
    VkImageView sampled_view() const noexcept{return sampled_view_;}
    bool sampled_image_initialized() const noexcept{return sampled_initialized_;}
    // Call only after successful submission of the first transition. The image
    // stays GENERAL for every later host write and GPU read across pool reuse.
    void mark_sampled_image_initialized() noexcept{if(sampled_image_)sampled_initialized_=true;}
    static std::shared_ptr<RgbaFrameStorage> allocate(VkDevice device,const VkPhysicalDeviceMemoryProperties& properties,std::size_t bytes){
        if(!device||!bytes||bytes>256ULL*1024*1024)return {};
        auto result=std::make_shared<VulkanRgbaFrameStorage>(device,bytes);
        VkBufferCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;ci.size=bytes;
        ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(device,&ci,nullptr,&result->buffer_)!=VK_SUCCESS)return {};
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,result->buffer_,&requirements);
        constexpr auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type=UINT32_MAX;
        for(std::uint32_t i=0;i<properties.memoryTypeCount;++i)
            if((requirements.memoryTypeBits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&required)==required){type=i;break;}
        if(type==UINT32_MAX)return {}; // Preserve the vector + staging fallback.
        VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
        if(vkAllocateMemory(device,&ai,nullptr,&result->memory_)!=VK_SUCCESS||
           vkBindBufferMemory(device,result->buffer_,result->memory_,0)!=VK_SUCCESS||
           vkMapMemory(device,result->memory_,0,VK_WHOLE_SIZE,0,&result->mapped_)!=VK_SUCCESS)return {};
        result->allocation_bytes_=requirements.size;result->memory_type_=type;
        return result;
    }
private:
    VkDevice device_{};VkBuffer buffer_{};VkDeviceMemory memory_{};void* mapped_=nullptr;std::size_t bytes_=0;
    VkDeviceSize allocation_bytes_=0;std::uint32_t memory_type_=UINT32_MAX;
    VkImage sampled_image_{};VkImageView sampled_view_{};
    unsigned sampled_width_=0,sampled_height_=0;
    bool sampled_probe_attempted_=false,sampled_initialized_=false;
};

inline RgbaFrameAllocator vulkan_rgba_frame_allocator(VkPhysicalDevice physical,VkDevice device){
    VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
    constexpr auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    bool available=false;
    for(std::uint32_t i=0;i<properties.memoryTypeCount;++i)available|=(properties.memoryTypes[i].propertyFlags&required)==required;
    if(!available)return {};
    // VkBuffer/VkDeviceMemory objects are independent per allocation. The
    // decoder thread may allocate/free these without touching XR state/queues.
    return [device,properties](std::size_t bytes){return VulkanRgbaFrameStorage::allocate(device,properties,bytes);};
}
} // namespace surfxr
