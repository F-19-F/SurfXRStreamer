#pragma once
#include "surfxr/foveation_renderer.hpp"
#include "surfxr/image_enhancement_renderer.hpp"
#include "surfxr/vulkan_rgba_storage.hpp"
#include "frame/xr/generated/image_enhancement_reference.hpp"
#include "frame/xr/generated/foveation_gamma2_reference.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace surfxr::tests::foveation_gpu {
constexpr VkFormat format=VK_FORMAT_R8G8B8A8_SRGB;
void require(bool ok,const char* message);
void check(VkResult result,const char* message);
template<class T>T info(VkStructureType type){T result{};result.sType=type;return result;}
struct Image {VkImage image{};VkImageView view{},raw_view{};VkDeviceMemory memory{};};
struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};};
struct Vulkan {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};unsigned family=0;
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    std::array<Image,2> sources{},targets{},extra_targets{};Buffer upload,readback;
    ~Vulkan(){
        if(device){
            vkDeviceWaitIdle(device);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            for(auto* images:{&sources,&targets,&extra_targets})for(auto& image:*images){
                if(image.raw_view)vkDestroyImageView(device,image.raw_view,nullptr);
                if(image.view)vkDestroyImageView(device,image.view,nullptr);
                if(image.image)vkDestroyImage(device,image.image,nullptr);
                if(image.memory)vkFreeMemory(device,image.memory,nullptr);
            }
            for(auto* buffer:{&upload,&readback}){
                if(buffer->buffer)vkDestroyBuffer(device,buffer->buffer,nullptr);
                if(buffer->memory)vkFreeMemory(device,buffer->memory,nullptr);
            }
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
    unsigned memory_type(unsigned bits,VkMemoryPropertyFlags flags){
        VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        for(unsigned i=0;i<properties.memoryTypeCount;++i)
            if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("memory_type_unavailable");
    }
    void initialize(bool vulkan11=false){
        auto app=info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);app.pApplicationName="surfxr-foveation-offscreen-test";app.apiVersion=vulkan11?VK_API_VERSION_1_1:VK_API_VERSION_1_0;
        auto ci=info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);ci.pApplicationInfo=&app;
        check(vkCreateInstance(&ci,nullptr,&instance),"vkCreateInstance");
        unsigned count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"vkEnumeratePhysicalDevices_count");require(count>0,"no_vulkan_device");
        std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"vkEnumeratePhysicalDevices");
        for(auto candidate:devices){
            VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(candidate,format,&properties);
            constexpr auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if((properties.optimalTilingFeatures&required)!=required)continue;
            unsigned n=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
            for(unsigned i=0;i<n;++i)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){physical=candidate;family=i;break;}
            if(physical)break;
        }
        require(physical!=VK_NULL_HANDLE,"no_srgb_graphics_device");const float priority=1;
        auto q=info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&priority;
        auto d=info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;
        check(vkCreateDevice(physical,&d,nullptr,&device),"vkCreateDevice");vkGetDeviceQueue(device,family,0,&queue);
        auto p=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&p,nullptr,&pool),"vkCreateCommandPool");
        auto a=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);a.commandPool=pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&a,&command),"vkAllocateCommandBuffers");
        auto f=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);check(vkCreateFence(device,&f,nullptr,&fence),"vkCreateFence");
    }
    void image(Image& image,unsigned width,unsigned height,VkImageUsageFlags usage,bool raw=false){
        auto ci=info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={width,height,1};
        if(raw)ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device,&ci,nullptr,&image.image),"vkCreateImage");VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image.image,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&image.memory),"vkAllocateMemory_image");check(vkBindImageMemory(device,image.image,image.memory,0),"vkBindImageMemory");
        auto vi=info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);vi.image=image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(vkCreateImageView(device,&vi,nullptr,&image.view),"vkCreateImageView");
        if(raw){vi.format=VK_FORMAT_R8G8B8A8_UNORM;check(vkCreateImageView(device,&vi,nullptr,&image.raw_view),"vkCreateImageView_raw");}
    }
    void buffer(Buffer& buffer,std::size_t bytes,VkBufferUsageFlags usage){
        auto ci=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);ci.size=bytes;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device,&ci,nullptr,&buffer.buffer),"vkCreateBuffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer.buffer,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;
        ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&buffer.memory),"vkAllocateMemory_buffer");check(vkBindBufferMemory(device,buffer.buffer,buffer.memory,0),"vkBindBufferMemory");
    }
    void begin(){check(vkResetCommandBuffer(command,0),"vkResetCommandBuffer");auto bi=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&bi),"vkBeginCommandBuffer");}
    void submit(){
        check(vkEndCommandBuffer(command),"vkEndCommandBuffer");check(vkResetFences(device,1,&fence),"vkResetFences");
        auto si=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si.commandBufferCount=1;si.pCommandBuffers=&command;
        check(vkQueueSubmit(queue,1,&si,fence),"vkQueueSubmit");check(vkWaitForFences(device,1,&fence,VK_TRUE,2'000'000'000ULL),"vkWaitForFences_2s");
    }
    void barrier(VkImage image,VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags before,VkPipelineStageFlags after){
        auto b=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);b.image=image;b.oldLayout=old_layout;b.newLayout=new_layout;
        b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,before,after,0,0,nullptr,0,nullptr,1,&b);
    }
    void upload_pixels(const std::vector<std::uint8_t>& bytes,unsigned width,unsigned height,bool first){
        void* mapped=nullptr;check(vkMapMemory(device,upload.memory,0,bytes.size(),0,&mapped),"vkMapMemory_upload");std::memcpy(mapped,bytes.data(),bytes.size());vkUnmapMemory(device,upload.memory);
        upload_buffer(upload.buffer,width,height,first);
    }
    void upload_buffer(VkBuffer source,unsigned width,unsigned height,bool first){
        begin();for(unsigned eye=0;eye<2;++eye){
            barrier(sources[eye].image,first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                first?0:VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                first?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=std::size_t{width}*height*4*eye;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
            vkCmdCopyBufferToImage(command,source,sources[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(sources[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            if(first)barrier(targets[eye].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }submit();
    }
    std::vector<std::uint8_t> read_pixels(unsigned width,unsigned height,bool extra=false){
        const auto& images=extra?extra_targets:targets;
        const auto eye_bytes=std::size_t{width}*height*4;begin();
        for(unsigned eye=0;eye<2;++eye){
            barrier(images[eye].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=eye_bytes*eye;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
            vkCmdCopyImageToBuffer(command,images[eye].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
            barrier(images[eye].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        auto b=info<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);b.buffer=readback.buffer;b.size=VK_WHOLE_SIZE;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);submit();
        std::vector<std::uint8_t> result(eye_bytes*2);void* mapped=nullptr;check(vkMapMemory(device,readback.memory,0,result.size(),0,&mapped),"vkMapMemory_readback");std::memcpy(result.data(),mapped,result.size());vkUnmapMemory(device,readback.memory);return result;
    }
};

// Independent forward mapping derived from the installed encoder shader, not
// the inverse helper or the GLSL under test. Input is an encoded texel center.
double original_coordinate(double p);
std::uint8_t encode_srgb(double linear);
double decode_srgb(std::uint8_t byte);
constexpr std::array<std::array<double,2>,4> markers{{{{.17,.22}},{{.72,.18}},{{.28,.73}},{{.82,.79}}}};
constexpr std::array<std::array<double,3>,4> colors{{{{.85,.025,.025}},{{.025,.85,.025}},{{.025,.025,.85}},{{.85,.7,.025}}}};
std::array<double,3> grid(double u,double v,unsigned eye);
std::vector<std::uint8_t> encoded_fixture(unsigned width,unsigned height,bool use_grid,bool linear_layout=false);
std::array<double,3> pixel_at(const std::vector<std::uint8_t>& pixels,unsigned width,unsigned height,unsigned eye,double u,double v);
unsigned parse_size(const char* value,unsigned minimum=256);
void run(unsigned sw,unsigned sh,unsigned width,unsigned height,bool linear_layout=false);
void run_focus(bool quad);
void run_nv12(bool packed=true);
void run_gaze_atlas(bool timewarp=false);
void run_timewarp();

void run_sharpen(unsigned sw,unsigned sh,unsigned width,unsigned height,float sharpness,unsigned frames,bool quad,
                 bool zero_weight_skip,const std::string& pattern);

// The same frozen D-byte corpus as --sharpen-check, kept separate from its
// existing reference/gates. Gamma2 must not use an E8 readback as its CPU input.
std::array<surfxr::enhancement_reference::Image,2> gamma2_fixtures(unsigned sw,unsigned sh,const std::string& pattern);
struct Gamma2Difference {
    std::uint64_t channels=0,different=0;double absolute=0,squared=0;unsigned maximum=0;
    std::array<std::uint64_t,256> histogram{};
    void add(unsigned a,unsigned b){
        const unsigned error=unsigned(std::abs(int(a)-int(b)));
        ++channels;different+=error!=0;absolute+=error;squared+=double(error)*error;
        maximum=std::max(maximum,error);++histogram[error];
    }
    double mean()const{return channels?absolute/channels:0;}
    unsigned percentile(double fraction)const{
        std::uint64_t sum=0;for(unsigned i=0;i<histogram.size();++i){sum+=histogram[i];if(sum>=channels*fraction)return i;}return 255;
    }
};
Gamma2Difference gamma2_difference(const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b);
std::vector<unsigned> gamma2_probe_axis(unsigned extent,bool full);
void run_gamma2(unsigned sw,unsigned sh,unsigned width,unsigned height,float sharpness,unsigned frames,bool quad,
                bool zero_weight_skip,const std::string& pattern);
// Isolated no-enhancement A/B: the production renderer and shader are shared.
// The candidate aliases the decoder's real buffer allocation and samples it
// in GENERAL, including a later CPU rewrite after a completed GPU read.
void run_linear_source_probe(unsigned sw,unsigned sh,unsigned width,unsigned height,unsigned frames);
}
