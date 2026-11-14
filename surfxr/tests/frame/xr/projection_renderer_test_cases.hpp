#pragma once
#include "surfxr/projection_renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace surfxr::tests::projection {
constexpr unsigned kDefaultTarget=512,kSourceWidth=512,kSourceHeight=256;
constexpr VkFormat kFormat=VK_FORMAT_R8G8B8A8_SRGB;
void require(bool value,const char* label);
void checked(VkResult result,const char* label);
bool near(float a,float b,float tolerance=.0001f);
std::array<float,4> transform(const std::array<float,16>& m,surfxr::UiVec3 p);
surfxr::UiVec3 ndc(const std::array<float,16>& m,surfxr::UiVec3 p);
unsigned cpu_tests();
struct Image {VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;};
struct Buffer {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;};
struct Vulkan {
    VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;
    unsigned family=0;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    Image source;std::array<Image,2> targets{},extra_sources{};Buffer upload,readback;
    ~Vulkan(){
        if(device){vkDeviceWaitIdle(device);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            for(auto* image:{&source,&targets[0],&targets[1],&extra_sources[0],&extra_sources[1]}){if(image->view)vkDestroyImageView(device,image->view,nullptr);if(image->image)vkDestroyImage(device,image->image,nullptr);if(image->memory)vkFreeMemory(device,image->memory,nullptr);}
            for(auto* buffer:{&upload,&readback}){if(buffer->buffer)vkDestroyBuffer(device,buffer->buffer,nullptr);if(buffer->memory)vkFreeMemory(device,buffer->memory,nullptr);}
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
    std::uint32_t memory_type(std::uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        for(std::uint32_t i=0;i<properties.memoryTypeCount;++i)if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("required_memory_type_unavailable");
    }
    void initialize(bool format_lists=false){
        VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="surfxr-projection-offline-test";app.apiVersion=format_lists?VK_API_VERSION_1_2:VK_API_VERSION_1_0;
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
        VkDeviceCreateInfo d{};d.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;checked(vkCreateDevice(physical,&d,nullptr,&device),"vkCreateDevice");vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=family;checked(vkCreateCommandPool(device,&p,nullptr,&pool),"vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;alloc.commandPool=pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;checked(vkAllocateCommandBuffers(device,&alloc,&command),"vkAllocateCommandBuffers");
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;checked(vkCreateFence(device,&f,nullptr,&fence),"vkCreateFence");
    }
    void image(Image& out,unsigned width,unsigned height,VkImageUsageFlags usage){
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=kFormat;i.extent={width,height,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        checked(vkCreateImage(device,&i,nullptr,&out.image),"vkCreateImage");VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,out.image,&requirements);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=requirements.size;a.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);checked(vkAllocateMemory(device,&a,nullptr,&out.memory),"vkAllocateMemory_image");checked(vkBindImageMemory(device,out.image,out.memory,0),"vkBindImageMemory");
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=out.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=kFormat;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};checked(vkCreateImageView(device,&view,nullptr,&out.view),"vkCreateImageView");
    }
    void buffer(Buffer& out,VkDeviceSize size,VkBufferUsageFlags usage){
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=size;b.usage=usage;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateBuffer(device,&b,nullptr,&out.buffer),"vkCreateBuffer");VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,out.buffer,&requirements);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=requirements.size;a.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);checked(vkAllocateMemory(device,&a,nullptr,&out.memory),"vkAllocateMemory_buffer");checked(vkBindBufferMemory(device,out.buffer,out.memory,0),"vkBindBufferMemory");
    }
    void begin(){checked(vkResetCommandBuffer(command,0),"vkResetCommandBuffer");VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&b),"vkBeginCommandBuffer");}
    void submit(){checked(vkEndCommandBuffer(command),"vkEndCommandBuffer");checked(vkResetFences(device,1,&fence),"vkResetFences");VkSubmitInfo info{};info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;info.commandBufferCount=1;info.pCommandBuffers=&command;checked(vkQueueSubmit(queue,1,&info,fence),"vkQueueSubmit");checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"vkWaitForFences");}
    void barrier(VkImage image,VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags source_access,VkAccessFlags target_access,VkPipelineStageFlags source_stage,VkPipelineStageFlags target_stage){
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=source_access;b.dstAccessMask=target_access;b.oldLayout=old_layout;b.newLayout=new_layout;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,source_stage,target_stage,0,0,nullptr,0,nullptr,1,&b);
    }
};
struct NewFile {
    std::string path;int descriptor=-1;bool completed=false;
    explicit NewFile(const char* requested):path(requested){descriptor=::open(requested,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(descriptor>=0,"preview_output_must_be_new_file");}
    ~NewFile(){if(descriptor>=0)::close(descriptor);if(!completed)::unlink(path.c_str());}
    void write(const void* bytes,std::size_t size){const auto* data=static_cast<const std::uint8_t*>(bytes);while(size){const auto n=::write(descriptor,data,size);if(n<0&&errno==EINTR)continue;require(n>0,"preview_write_failed");data+=n;size-=static_cast<std::size_t>(n);}}
    void finish(){require(::fsync(descriptor)==0,"preview_fsync_failed");const int result=::close(descriptor);descriptor=-1;require(result==0,"preview_close_failed");completed=true;}
};
bool quadrant_color(const std::uint8_t* p,unsigned quadrant);
void gpu_preview(const char* path,unsigned target,unsigned max_samples,surfxr::ProjectionSampling sampling);
double linear_srgb(unsigned value);
unsigned encoded_srgb(double value);
// Independent level-zero reference. Magnification must stay ordinary bilinear
// sampling, without an extra blur kernel or a display-phase-dependent offset.
std::array<unsigned,4> reference_small_quality(const std::vector<std::uint8_t>& data,double u,double v);
struct ReferenceMip {unsigned width,height;std::vector<double> linear;};
std::vector<ReferenceMip> reference_mips(const std::vector<std::uint8_t>& rgba,unsigned width,unsigned height);
double reference_sample(const std::vector<ReferenceMip>& mips,double u,double v,double lod);
// Historical shader only in the scalar comparison: alternating diagonal,
// signed four taps, and half derivatives (one finer mip level).
double legacy_quality_sample(const std::vector<ReferenceMip>& mips,double u,double v,double rho,unsigned phase);
double reference_box(const ReferenceMip& source,double u,double v,double rho);
void grayscale_fixture(std::vector<std::uint8_t>& rgba,unsigned width,unsigned height,unsigned pattern);
void gpu_quality_test();
void gpu_nv12_test();
void gpu_benchmark(unsigned target,surfxr::ProjectionSampling sampling,bool dirty,unsigned frames,bool timing);
}
