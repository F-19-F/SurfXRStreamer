#pragma once
#include "surfxr/nv12_frame.hpp"
#include <vulkan/vulkan.h>
#include <cstdlib>
#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#endif

namespace surfxr {
// Two copied host images, or plane views of one dedicated imported NV12 image.
// Only the rendering thread transitions layouts; callers retain the complete
// immutable VideoFrame until its last GPU read finishes. No queue use here.
class VulkanNv12FrameStorage final:public Nv12FrameStorage {
    VkDevice device_{};
    std::array<VkImage,2> images_{};
    std::array<VkImageView,2> views_{};
    std::array<VkDeviceMemory,2> memory_{};
    std::array<void*,2> mapped_{};
    std::array<std::size_t,2> offset_{};
    bool initialized_=false;
    bool foreign_=false;
    std::shared_ptr<void> backing_;
    explicit VulkanNv12FrameStorage(VkDevice device):device_(device){}
public:
    ~VulkanNv12FrameStorage()override{
        for(auto view:views_)if(view)vkDestroyImageView(device_,view,nullptr);
        for(unsigned p=0;p<2;++p){
            if(images_[p]&&(!p||images_[p]!=images_[0]))vkDestroyImage(device_,images_[p],nullptr);
            if(mapped_[p]&&!foreign_)vkUnmapMemory(device_,memory_[p]);
            if(memory_[p])vkFreeMemory(device_,memory_[p],nullptr);
        }
    }
    std::uint8_t* data(unsigned p)noexcept override{return static_cast<std::uint8_t*>(mapped_[p])+offset_[p];}
    VkDevice device()const noexcept{return device_;}
    VkImage image(unsigned p)const noexcept{return images_[p];}
    VkImageView view(unsigned p)const noexcept{return views_[p];}
    bool initialized()const noexcept{return initialized_;}
    bool foreign()const noexcept{return foreign_;}
    void mark_initialized()noexcept{initialized_=true;}
    static std::shared_ptr<Nv12FrameStorage> allocate(VkPhysicalDevice physical,VkDevice device,unsigned w,unsigned h){
        if(!physical||!device||!w||!h||w>8192||h>8192||w%2||h%2||std::uint64_t{w}*h>32u*1024u*1024u)return {};
        VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        auto storage=std::shared_ptr<VulkanNv12FrameStorage>(new VulkanNv12FrameStorage(device));storage->width=w;storage->height=h;
        for(unsigned p=0;p<2;++p){
            const auto format=p?VK_FORMAT_R8G8_UNORM:VK_FORMAT_R8_UNORM;
            VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;
            ci.extent={p?w/2:w,p?h/2:h,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
            ci.tiling=VK_IMAGE_TILING_LINEAR;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
            if(vkCreateImage(device,&ci,nullptr,&storage->images_[p])!=VK_SUCCESS)return {};
            VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,storage->images_[p],&req);
            constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            unsigned type=UINT32_MAX;
            for(unsigned i=0;i<properties.memoryTypeCount;++i)
                if((req.memoryTypeBits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
            if(type==UINT32_MAX)return {};
            VkMemoryDedicatedAllocateInfo dedicated{};dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;dedicated.image=storage->images_[p];
            VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.pNext=&dedicated;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
            if(vkAllocateMemory(device,&ai,nullptr,&storage->memory_[p])!=VK_SUCCESS||
               vkBindImageMemory(device,storage->images_[p],storage->memory_[p],0)!=VK_SUCCESS||
               vkMapMemory(device,storage->memory_[p],0,VK_WHOLE_SIZE,0,&storage->mapped_[p])!=VK_SUCCESS)return {};
            const VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device,storage->images_[p],&subresource,&layout);
            if(layout.offset>req.size||layout.rowPitch<w||layout.rowPitch*(p?h/2:h)>req.size-layout.offset)return {};
            storage->offset_[p]=layout.offset;storage->stride[p]=layout.rowPitch;storage->bytes[p]=req.size-layout.offset;
            VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=storage->images_[p];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
            vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(device,&vi,nullptr,&storage->views_[p])!=VK_SUCCESS)return {};
        }
        return storage;
    }
    static std::shared_ptr<Nv12FrameStorage> import(VkPhysicalDevice physical,VkDevice device,const Nv12DmaBuf& source){
#ifdef __linux__
        (void)physical;
        if(source.fd<0||!source.backing||!source.mapping||!source.width||!source.height||source.width%2||source.height%2||
           source.width>8192||source.height>8192||source.row_pitch<source.width||source.y_offset>source.uv_offset||
           source.uv_offset>source.size||source.row_pitch*source.height>source.uv_offset-source.y_offset||
           source.row_pitch*(source.height/2)>source.size-source.uv_offset)return {};
        auto s=std::shared_ptr<VulkanNv12FrameStorage>(new VulkanNv12FrameStorage(device));
        s->foreign_=true;s->backing_=source.backing;s->width=source.width;s->height=source.height;
        s->stride={source.row_pitch,source.row_pitch};s->bytes={source.uv_offset-source.y_offset,source.size-source.uv_offset};
        s->mapped_={source.mapping,source.mapping};s->offset_={source.y_offset,source.uv_offset};
        std::array<VkSubresourceLayout,2> planes{};
        planes[0].offset=source.y_offset;planes[1].offset=source.uv_offset;
        planes[0].rowPitch=planes[1].rowPitch=source.row_pitch;
        VkImageDrmFormatModifierExplicitCreateInfoEXT layout{};layout.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        layout.drmFormatModifier=0;layout.drmFormatModifierPlaneCount=2;layout.pPlaneLayouts=planes.data();
        VkExternalMemoryImageCreateInfo external{};external.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;external.pNext=&layout;
        VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.pNext=&external;ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;ci.extent={source.width,source.height,1};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
        if(vkCreateImage(device,&ci,nullptr,&s->images_[0])!=VK_SUCCESS)return {};
        s->images_[1]=s->images_[0];
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,s->images_[0],&req);
        auto get_fd_properties=reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR"));
        VkMemoryFdPropertiesKHR props{};props.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if(!get_fd_properties||get_fd_properties(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,source.fd,&props)!=VK_SUCCESS)return {};
        const auto bits=req.memoryTypeBits&props.memoryTypeBits;if(!bits||req.size>source.size)return {};
        unsigned type=0;while(!(bits&(1u<<type)))++type;
        const int fd=fcntl(source.fd,F_DUPFD_CLOEXEC,0);if(fd<0)return {};
        VkImportMemoryFdInfoKHR import{};import.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;import.fd=fd;
        VkMemoryDedicatedAllocateInfo dedicated{};dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image=s->images_[0];dedicated.pNext=&import;
        VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.pNext=&dedicated;ai.allocationSize=source.size;ai.memoryTypeIndex=type;
        if(vkAllocateMemory(device,&ai,nullptr,&s->memory_[0])!=VK_SUCCESS){close(fd);return {};}
        if(vkBindImageMemory(device,s->images_[0],s->memory_[0],0)!=VK_SUCCESS)return {};
        for(unsigned p=0;p<2;++p){
            VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=s->images_[0];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
            vi.format=p?VK_FORMAT_R8G8_UNORM:VK_FORMAT_R8_UNORM;
            vi.subresourceRange={p?VK_IMAGE_ASPECT_PLANE_1_BIT:VK_IMAGE_ASPECT_PLANE_0_BIT,0,1,0,1};
            if(vkCreateImageView(device,&vi,nullptr,&s->views_[p])!=VK_SUCCESS)return {};
        }
        return s;
#else
        (void)physical;(void)device;(void)source;return {};
#endif
    }
};
inline Nv12FrameAllocator vulkan_nv12_frame_allocator(VkPhysicalDevice physical,VkDevice device,bool dmabuf=false){
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    if(properties.apiVersion<VK_API_VERSION_1_1)return {};
    for(const auto format:{VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8_UNORM}){
        VkFormatProperties caps{};vkGetPhysicalDeviceFormatProperties(physical,format,&caps);
        constexpr auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((caps.linearTilingFeatures&required)!=required)return {};
        VkImageFormatProperties supported{};
        if(vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_SAMPLED_BIT,0,&supported)!=VK_SUCCESS||
           supported.maxExtent.width<8192||supported.maxExtent.height<8192)return {};
    }
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    bool available=false;
    for(unsigned i=0;i<memory.memoryTypeCount;++i)available|=(memory.memoryTypes[i].propertyFlags&flags)==flags;
    if(!available)return {};
    Nv12FrameAllocator allocator=[physical,device](unsigned w,unsigned h){return VulkanNv12FrameStorage::allocate(physical,device,w,h);};
    const auto* override=std::getenv("SURFXR_NV12_DMABUF");
    if(dmabuf&&(!override||std::strcmp(override,"0")!=0))
        allocator.import=[physical,device](const Nv12DmaBuf& source){return VulkanNv12FrameStorage::import(physical,device,source);};
    return allocator;
}
}
