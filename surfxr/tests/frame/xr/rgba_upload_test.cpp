#include "surfxr/rgba_upload.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#ifdef SURFXR_UPLOAD_VULKAN_TEST
#include <vulkan/vulkan.h>
#include "surfxr/vulkan_rgba_storage.hpp"
#endif
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
// Independent reference to the previous visual result, including small crops.
void reference(std::uint8_t* dst,const std::uint8_t* src,std::size_t stride,unsigned w,unsigned h,bool bgra,bool feather){
    for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col){
        const auto* p=src+std::size_t{row}*stride+std::size_t{col}*4;
        auto* q=dst+(std::size_t{row}*w+col)*4;
        q[0]=p[bgra?2:0];q[1]=p[1];q[2]=p[bgra?0:2];q[3]=p[3];
        const auto edge=std::min({col,row,w-1-col,h-1-row});
        if(feather&&edge<2)q[3]=static_cast<std::uint8_t>((unsigned(p[3])*(edge?192u:64u)+127u)/255u);
    }
}
unsigned self_test(){
    unsigned count=0;
    const std::array<std::array<unsigned,2>,11> sizes{{{{1,1}},{{1,9}},{{9,1}},{{2,2}},{{3,3}},{{15,7}},{{16,8}},{{17,9}},{{31,37}},{{2560,1440}},{{1440,2560}}}};
    for(const auto& size:sizes)for(bool crop:{false,true})for(bool bgra:{false,true})for(bool feather:{false,true}){
        const auto w=size[0],h=size[1];const unsigned x=crop?3:0,y=crop?2:0;
        const auto stride=std::size_t{w+x}*4+(crop?19:0);
        std::vector<std::uint8_t> src(stride*(h+y));
        std::uint32_t random=0x813198du;
        for(auto& value:src){random^=random<<13;random^=random>>17;random^=random<<5;value=static_cast<std::uint8_t>(random);}
        const auto saved=src;const auto bytes=std::size_t{w}*h*4;
        std::vector<std::uint8_t> actual(bytes+32,0x5a),expected(bytes);
        const auto* source=src.data()+y*stride+x*4;
        reference(expected.data(),source,stride,w,h,bgra,feather);
        surfxr::rgba_upload_copy(actual.data()+16,source,stride,w,h,bgra,feather);
        require(std::equal(expected.begin(),expected.end(),actual.begin()+16),"pixel_result_changed");
        require(std::all_of(actual.begin(),actual.begin()+16,[](auto b){return b==0x5a;})&&
                std::all_of(actual.end()-16,actual.end(),[](auto b){return b==0x5a;}),"destination_guard_changed");
        require(src==saved,"immutable_source_changed");++count;
        if(!bgra&&feather){
            const auto plan=surfxr::rgba_upload_edge_plan(w,h);
            require(plan.valid()==(w>4&&h>4),"edge_plan_fallback_mismatch");
            if(plan.valid()){
                std::vector<std::uint8_t> strips(plan.bytes+32,0x5a),assembled(bytes,0),coverage(std::size_t{w}*h,0);
                const auto written=surfxr::rgba_upload_edge_copy(strips.data()+16,source,stride,w,h);
                require(written.bytes==plan.bytes&&plan.bytes==std::size_t{w+h-4}*16,"edge_plan_byte_count_mismatch");
                // Emulate the exact non-overlapping buffer-to-image regions;
                // compare to the independent per-pixel feather/crop reference.
                for(unsigned row=2;row<h-2;++row)for(unsigned col=2;col<w-2;++col){
                    const auto at=std::size_t{row}*w+col;++coverage[at];
                    std::memcpy(assembled.data()+at*4,source+std::size_t{row}*stride+col*4,4);
                }
                for(const auto& region:plan.regions){require(region.offset%4==0,"edge_region_offset_unaligned");
                    for(unsigned row=0;row<region.height;++row)for(unsigned col=0;col<region.width;++col){
                        const auto at=std::size_t{region.y+row}*w+region.x+col;++coverage[at];
                        std::memcpy(assembled.data()+at*4,strips.data()+16+region.offset+(std::size_t{row}*region.width+col)*4,4);
                    }
                }
                require(assembled==expected,"direct_interior_edge_result_changed");
                require(std::all_of(coverage.begin(),coverage.end(),[](auto n){return n==1;}),"direct_edge_regions_overlap_or_leave_gaps");
                require(std::all_of(strips.begin(),strips.begin()+16,[](auto b){return b==0x5a;})&&
                        std::all_of(strips.end()-16,strips.end(),[](auto b){return b==0x5a;}),"edge_destination_guard_changed");
                require(src==saved,"edge_copy_mutated_source");++count;
            }
        }
    }
    // Exercise every possible alpha, including fully transparent pixels.
    for(unsigned alpha=0;alpha<256;++alpha){
        constexpr unsigned w=5,h=7;std::array<std::uint8_t,w*h*4> source{};
        for(std::size_t i=0;i<source.size();i+=4){source[i]=31;source[i+1]=97;source[i+2]=211;source[i+3]=std::uint8_t(alpha);}
        const auto plan=surfxr::rgba_upload_edge_plan(w,h);std::vector<std::uint8_t> strips(plan.bytes),expected(source.size());
        reference(expected.data(),source.data(),w*4,w,h,false,true);
        surfxr::rgba_upload_edge_copy(strips.data(),source.data(),w*4,w,h);
        for(const auto& region:plan.regions)for(unsigned y=0;y<region.height;++y)for(unsigned x=0;x<region.width;++x)
            require(std::memcmp(strips.data()+region.offset+(std::size_t{y}*region.width+x)*4,
                    expected.data()+(std::size_t{y+region.y}*w+x+region.x)*4,4)==0,"edge_arbitrary_alpha_changed");
        ++count;
    }
    return count;
}
#ifdef SURFXR_UPLOAD_VULKAN_TEST
template<class T>T info(VkStructureType type){T v{};v.sType=type;return v;}
void check(VkResult result,const char* name){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(name)+":"+std::to_string(result));}
struct Device {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
    unsigned family=0;
    Device(){
        auto ci=info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        check(vkCreateInstance(&ci,nullptr,&instance),"instance");
        std::uint32_t count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"physical_count");require(count>0,"no_vulkan_device");
        std::vector<VkPhysicalDevice> all(count);check(vkEnumeratePhysicalDevices(instance,&count,all.data()),"physical");physical=all.front();
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
        while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;
        require(family<count,"no_graphics_queue");
        const float priority=1;auto qi=info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        auto di=info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
        check(vkCreateDevice(physical,&di,nullptr,&device),"device");
    }
    ~Device(){if(device)vkDestroyDevice(device,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}
};
struct Upload {
    VkDevice device{};VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};
    explicit Upload(VkDevice d):device(d){}
    ~Upload(){if(mapped)vkUnmapMemory(device,memory);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);}
};
struct DirectCommands {
    Device& vk;VkQueue queue{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    explicit DirectCommands(Device& d):vk(d){}
    ~DirectCommands(){vkDeviceWaitIdle(vk.device);if(fence)vkDestroyFence(vk.device,fence,nullptr);if(pool)vkDestroyCommandPool(vk.device,pool,nullptr);}
    void initialize(){
        vkGetDeviceQueue(vk.device,vk.family,0,&queue);
        auto pi=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);pi.queueFamilyIndex=vk.family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(vk.device,&pi,nullptr,&pool),"direct_command_pool");
        auto ai=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
        check(vkAllocateCommandBuffers(vk.device,&ai,&command),"direct_command_buffer");
        auto fi=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);check(vkCreateFence(vk.device,&fi,nullptr,&fence),"direct_fence");
    }
    void begin(){
        check(vkResetCommandBuffer(command,0),"direct_reset_command");check(vkResetFences(vk.device,1,&fence),"direct_reset_fence");
        auto bi=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&bi),"direct_begin");
    }
    double submit(){
        check(vkEndCommandBuffer(command),"direct_end");auto si=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);si.commandBufferCount=1;si.pCommandBuffers=&command;
        const auto before=std::chrono::steady_clock::now();
        check(vkQueueSubmit(queue,1,&si,fence),"direct_submit");
        const auto result=vkWaitForFences(vk.device,1,&fence,VK_TRUE,2'000'000'000ULL);
        if(result!=VK_SUCCESS)vkDeviceWaitIdle(vk.device); // Protect per-case allocations on exception too.
        check(result,"direct_wait");
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
    }
};
struct DirectImage {
    VkDevice device{};VkImage image{};VkDeviceMemory memory{};
    explicit DirectImage(VkDevice d):device(d){}
    ~DirectImage(){if(image)vkDestroyImage(device,image,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);}
};
void gpu_direct_test(){
    Device vk;DirectCommands commands(vk);commands.initialize();
    VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(vk.physical,&properties);
    const auto memory_type=[&](unsigned bits,VkMemoryPropertyFlags flags){
        for(unsigned i=0;i<properties.memoryTypeCount;++i)if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("direct_test_memory_type_unavailable");
    };
    const auto buffer=[&](Upload& out,std::size_t bytes,VkBufferUsageFlags usage){
        auto bi=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);bi.size=bytes;bi.usage=usage;
        check(vkCreateBuffer(vk.device,&bi,nullptr,&out.buffer),"direct_buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(vk.device,out.buffer,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;
        ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(vk.device,&ai,nullptr,&out.memory),"direct_buffer_memory");check(vkBindBufferMemory(vk.device,out.buffer,out.memory,0),"direct_buffer_bind");
        check(vkMapMemory(vk.device,out.memory,0,VK_WHOLE_SIZE,0,&out.mapped),"direct_buffer_map");
    };
    struct Case {unsigned width,height,x,y,padding;bool bgra;};
    const std::array<Case,11> cases{{{17,9,0,0,0,false},{17,9,3,2,12,false},{31,37,1,3,28,false},
        {257,19,3,2,20,false},{5,5,2,1,4,false},{1,1,3,2,4,false},{4,9,2,1,8,false},
        {9,4,1,2,12,false},{3,3,0,0,0,false},{17,9,3,2,19,false},{17,9,3,2,12,true}}};
    unsigned direct_cases=0,fallback_cases=0;std::uint64_t compared_bytes=0;std::array<bool,256> edge_alphas{};
    for(const auto& test:cases){
        const auto w=test.width,h=test.height,source_width=w+test.x+3,source_height=h+test.y+2;
        const std::size_t stride=std::size_t{source_width}*4+test.padding,bytes=std::size_t{w}*h*4;
        // Use the actual decoder allocation class, not an ordinary copied
        // staging buffer, to exercise its cached/coherent transfer mapping.
        auto storage=surfxr::VulkanRgbaFrameStorage::allocate(vk.device,properties,stride*source_height);
        require(bool(storage),"direct_decoder_storage_unavailable");
        auto* decoder=dynamic_cast<surfxr::VulkanRgbaFrameStorage*>(storage.get());require(decoder,"direct_decoder_storage_type");
        auto* src=storage->data();std::uint32_t random=0x7623a91u;
        for(std::size_t i=0;i<storage->size();++i){random^=random<<13;random^=random>>17;random^=random<<5;src[i]=std::uint8_t(random);}
        for(unsigned y=0;y<source_height;++y)for(unsigned x=0;x<source_width;++x)src[y*stride+x*4+3]=std::uint8_t((x+37*y)&255u);
        const std::vector<std::uint8_t> saved(src,src+storage->size());
        const auto* crop=src+test.y*stride+test.x*4;
        std::vector<std::uint8_t> expected(bytes),independent(bytes);
        surfxr::rgba_upload_copy(expected.data(),crop,stride,w,h,test.bgra,true);
        reference(independent.data(),crop,stride,w,h,test.bgra,true);require(expected==independent,"direct_cpu_reference_disagreement");
        const auto plan=surfxr::rgba_upload_edge_plan(w,h);
        const bool direct=plan.valid()&&stride%4==0&&!test.bgra;
        Upload patch(vk.device),readback(vk.device);const auto patch_bytes=direct?plan.bytes:bytes;
        buffer(patch,patch_bytes+32,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);buffer(readback,bytes+32,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memset(patch.mapped,0x5a,patch_bytes+32);std::memset(readback.mapped,0xa7,bytes+32);
        auto* patch_data=static_cast<std::uint8_t*>(patch.mapped)+16;
        if(direct){surfxr::rgba_upload_edge_copy(patch_data,crop,stride,w,h);++direct_cases;
            for(const auto& part:plan.regions)for(unsigned y=0;y<part.height;++y)for(unsigned x=0;x<part.width;++x)
                edge_alphas[crop[(part.y+y)*stride+(part.x+x)*4+3]]=true;
        }else{surfxr::rgba_upload_copy(patch_data,crop,stride,w,h,test.bgra,true);++fallback_cases;}
        DirectImage target(vk.device);
        auto ii=info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);ii.imageType=VK_IMAGE_TYPE_2D;
        ii.format=test.bgra?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_R8G8B8A8_SRGB;ii.extent={w,h,1};ii.mipLevels=ii.arrayLayers=1;
        ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(vk.device,&ii,nullptr,&target.image),"direct_image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(vk.device,target.image,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(vk.device,&ai,nullptr,&target.memory),"direct_image_memory");check(vkBindImageMemory(vk.device,target.image,target.memory,0),"direct_image_bind");
        commands.begin();auto barrier=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=target.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        if(direct){
            region.bufferOffset=std::size_t{test.y+2}*stride+std::size_t{test.x+2}*4;region.bufferRowLength=static_cast<unsigned>(stride/4);
            region.imageOffset={2,2,0};region.imageExtent={w-4,h-4,1};
            vkCmdCopyBufferToImage(commands.command,decoder->buffer(),target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            std::array<VkBufferImageCopy,4> strips{};
            for(unsigned i=0;i<strips.size();++i){const auto& part=plan.regions[i];auto& copy=strips[i];
                copy.bufferOffset=16+part.offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                copy.imageOffset={int(part.x),int(part.y),0};copy.imageExtent={part.width,part.height,1};}
            vkCmdCopyBufferToImage(commands.command,patch.buffer,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,strips.size(),strips.data());
        }else{region.bufferOffset=16;region.imageExtent={w,h,1};vkCmdCopyBufferToImage(commands.command,patch.buffer,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);}
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        region={};region.bufferOffset=16;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={w,h,1};
        vkCmdCopyImageToBuffer(commands.command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&region);
        auto host=info<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback.buffer;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);commands.submit();
        const auto* actual=static_cast<const std::uint8_t*>(readback.mapped);
        require(std::memcmp(actual+16,expected.data(),bytes)==0,"direct_gpu_pixels_differ_from_original_upload");
        require(std::memcmp(src,saved.data(),saved.size())==0,"direct_gpu_upload_changed_decoder_storage");
        const auto* guarded_patch=static_cast<const std::uint8_t*>(patch.mapped);
        for(unsigned i=0;i<16;++i)require(actual[i]==0xa7&&actual[bytes+16+i]==0xa7&&guarded_patch[i]==0x5a&&guarded_patch[patch_bytes+16+i]==0x5a,"direct_gpu_upload_guard_changed");
        compared_bytes+=bytes;
    }
    const auto alphas=static_cast<unsigned>(std::count(edge_alphas.begin(),edge_alphas.end(),true));require(alphas==256,"direct_gpu_alpha_coverage_incomplete");
    std::cout<<"{\"status\":\"gpu_direct_upload_passed\",\"cases\":"<<cases.size()<<",\"direct_cases\":"<<direct_cases
        <<",\"fallback_cases\":"<<fallback_cases<<",\"compared_rgba_bytes\":"<<compared_bytes<<",\"edge_alpha_values\":"<<alphas
        <<",\"byte_identical\":true,\"decoder_source_immutable\":true,\"network_used\":false,\"openxr_session_started\":false}\n";
}
void gpu_direct_benchmark(){
    Device vk;DirectCommands commands(vk);commands.initialize();
    VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(vk.physical,&properties);
    const auto memory_type=[&](unsigned bits,VkMemoryPropertyFlags flags){
        for(unsigned i=0;i<properties.memoryTypeCount;++i)if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("direct_benchmark_memory_type_unavailable");
    };
    const auto buffer=[&](Upload& out,std::size_t bytes,VkBufferUsageFlags usage){
        auto bi=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);bi.size=bytes;bi.usage=usage;
        check(vkCreateBuffer(vk.device,&bi,nullptr,&out.buffer),"benchmark_buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(vk.device,out.buffer,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;
        ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(vk.device,&ai,nullptr,&out.memory),"benchmark_buffer_memory");check(vkBindBufferMemory(vk.device,out.buffer,out.memory,0),"benchmark_buffer_bind");
        check(vkMapMemory(vk.device,out.memory,0,VK_WHOLE_SIZE,0,&out.mapped),"benchmark_buffer_map");
        return ai.memoryTypeIndex;
    };
    const auto summary=[](const std::vector<double>& samples){
        auto sorted=samples;std::sort(sorted.begin(),sorted.end());double total=0;for(auto value:sorted)total+=value;
        std::cout<<"{\"mean\":"<<total/sorted.size()<<",\"median\":"<<(sorted[sorted.size()/2-1]+sorted[sorted.size()/2])*.5
                 <<",\"min\":"<<sorted.front()<<",\"max\":"<<sorted.back()<<",\"samples\":[";
        for(std::size_t i=0;i<samples.size();++i){if(i)std::cout<<',';std::cout<<samples[i];}std::cout<<"]}";
    };
    constexpr unsigned iterations=20,warmup=4;
    std::cout<<"{\"status\":\"gpu_direct_benchmark_passed\",\"iterations_per_variant\":"<<iterations
        <<",\"warmup_per_variant\":"<<warmup<<",\"order\":\"alternating_AB_BA\",\"pattern\":\"deterministic_random_rgba\""
        <<",\"gpu_wall_is_submit_fence_not_timestamp\":true,\"readback_outside_timing\":true,\"network_used\":false,\"openxr_session_started\":false,\"cases\":[";
    bool first=true;
    for(const auto& size:std::array<std::array<unsigned,2>,2>{{{{2560,1440}},{{1440,2560}}}}){
        const auto w=size[0],h=size[1];const std::size_t stride=std::size_t{w}*4,bytes=stride*h;
        std::vector<std::uint8_t> source(bytes),expected(bytes);std::uint32_t random=0x5723219u;
        for(auto& value:source){random^=random<<13;random^=random>>17;random^=random<<5;value=std::uint8_t(random);}
        reference(expected.data(),source.data(),stride,w,h,false,true);
        auto storage=surfxr::VulkanRgbaFrameStorage::allocate(vk.device,properties,bytes);require(bool(storage),"benchmark_decoder_storage_unavailable");
        auto* decoder=dynamic_cast<surfxr::VulkanRgbaFrameStorage*>(storage.get());require(decoder,"benchmark_decoder_storage_type");
        std::memcpy(storage->data(),source.data(),bytes);const auto plan=surfxr::rgba_upload_edge_plan(w,h);
        Upload full(vk.device),edges(vk.device),readback(vk.device);
        const auto full_type=buffer(full,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT),edge_type=buffer(edges,plan.bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        buffer(readback,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        DirectImage target(vk.device);auto ii=info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_SRGB;ii.extent={w,h,1};ii.mipLevels=ii.arrayLayers=1;
        ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        // Preserve the non-mutable, sampled/color-attachment upload target;
        // transfer-source is needed only for the untimed byte comparison.
        ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(vk.device,&ii,nullptr,&target.image),"benchmark_image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(vk.device,target.image,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(vk.device,&ai,nullptr,&target.memory),"benchmark_image_memory");check(vkBindImageMemory(vk.device,target.image,target.memory,0),"benchmark_image_bind");
        bool initialized=false;std::array<std::vector<double>,2> cpu_times,gpu_times;
        const auto render=[&](bool direct,bool verify){
            const auto before=std::chrono::steady_clock::now();
            if(direct)surfxr::rgba_upload_edge_copy(static_cast<std::uint8_t*>(edges.mapped),storage->data(),stride,w,h);
            else surfxr::rgba_upload_copy(static_cast<std::uint8_t*>(full.mapped),source.data(),stride,w,h,false,true);
            const auto cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            commands.begin();auto barrier=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=target.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            barrier.oldLayout=initialized?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask=initialized?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT:0;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands.command,initialized?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={w,h,1};
            if(direct){
                region.bufferOffset=2*stride+8;region.bufferRowLength=w;region.imageOffset={2,2,0};region.imageExtent={w-4,h-4,1};
                vkCmdCopyBufferToImage(commands.command,decoder->buffer(),target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
                std::array<VkBufferImageCopy,4> strips{};
                for(unsigned i=0;i<strips.size();++i){const auto& part=plan.regions[i];auto& copy=strips[i];
                    copy.bufferOffset=part.offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    copy.imageOffset={int(part.x),int(part.y),0};copy.imageExtent={part.width,part.height,1};}
                vkCmdCopyBufferToImage(commands.command,edges.buffer,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,strips.size(),strips.data());
            }else vkCmdCopyBufferToImage(commands.command,full.buffer,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            if(verify){
                barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                region={};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={w,h,1};
                vkCmdCopyImageToBuffer(commands.command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&region);
                auto host=info<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback.buffer;host.size=VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
                barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            }
            barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commands.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            const auto gpu_ms=commands.submit();initialized=true;
            if(verify)require(std::memcmp(readback.mapped,expected.data(),bytes)==0,"benchmark_upload_pixels_changed");
            return std::array<double,2>{{cpu_ms,gpu_ms}};
        };
        for(unsigned iteration=0;iteration<warmup+iterations;++iteration)for(unsigned order=0;order<2;++order){
            const auto variant=(iteration+order)%2;const auto measured=render(variant!=0,false);
            if(iteration>=warmup){cpu_times[variant].push_back(measured[0]);gpu_times[variant].push_back(measured[1]);}
        }
        render(false,true);render(true,true);require(std::memcmp(storage->data(),source.data(),bytes)==0,"benchmark_decoder_source_mutated");
        if(!first)std::cout<<',';
        first=false;
        std::cout<<"{\"width\":"<<w<<",\"height\":"<<h<<",\"frame_bytes\":"<<bytes<<",\"edge_bytes\":"<<plan.bytes
            <<",\"staging_memory_flags\":"<<properties.memoryTypes[full_type].propertyFlags<<",\"edge_memory_flags\":"<<properties.memoryTypes[edge_type].propertyFlags
            <<",\"byte_identical\":true,\"decoder_source_immutable\":true,\"variants\":[";
        for(unsigned variant=0;variant<2;++variant){if(variant)std::cout<<',';
            std::cout<<"{\"variant\":\""<<(variant?"direct_interior_four_edges":"full_staging_copy")<<"\",\"regions\":"<<(variant?5:1)<<",\"cpu_copy_ms\":";
            summary(cpu_times[variant]);std::cout<<",\"submit_fence_wall_ms\":";summary(gpu_times[variant]);std::cout<<'}';}
        std::cout<<"]}";
    }
    std::cout<<"]}\n";
}
void previous(std::uint8_t* dst,const std::uint8_t* src,unsigned w,unsigned h,bool bgra){
    const std::size_t stride=std::size_t{w}*4;
    for(unsigned row=0;row<h;++row)std::memcpy(dst+row*stride,src+row*stride,stride);
    const auto edge_pixel=[&](unsigned col,unsigned row){const auto edge=std::min({col,row,w-1-col,h-1-row});
        if(edge<2){auto& alpha=dst[row*stride+col*4+3];alpha=static_cast<std::uint8_t>((unsigned(alpha)*(edge?192u:64u)+127u)/255u);}};
    for(unsigned row=0;row<h;++row){if(row<2||row+2>=h)for(unsigned col=0;col<w;++col)edge_pixel(col,row);
        else{edge_pixel(0,row);edge_pixel(1,row);edge_pixel(w-2,row);edge_pixel(w-1,row);}}
    if(bgra)for(std::size_t at=0;at<stride*h;at+=4)std::swap(dst[at],dst[at+2]);
}
void benchmark(){
    Device vk;VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(vk.physical,&properties);
    std::cout<<"{\"status\":\"benchmark_passed\",\"openxr_session_started\":false,\"network_used\":false,\"cases\":[";
    bool first=true;
    for(const auto size:std::array<std::array<unsigned,2>,2>{{{{2560,1440}},{{1440,2560}}}}){
        const auto w=size[0],h=size[1];const auto bytes=std::size_t{w}*h*4;std::vector<std::uint8_t> src(bytes,127),expected(bytes);
        for(unsigned type=0;type<properties.memoryTypeCount;++type){
            const auto flags=properties.memoryTypes[type].propertyFlags;if(!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
            Upload upload(vk.device);auto bi=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            check(vkCreateBuffer(vk.device,&bi,nullptr,&upload.buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(vk.device,upload.buffer,&req);
            if(!(req.memoryTypeBits&(1u<<type)))continue;
            auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;ai.memoryTypeIndex=type;
            check(vkAllocateMemory(vk.device,&ai,nullptr,&upload.memory),"allocate");check(vkBindBufferMemory(vk.device,upload.buffer,upload.memory,0),"bind");
            check(vkMapMemory(vk.device,upload.memory,0,VK_WHOLE_SIZE,0,&upload.mapped),"map");
            auto range=info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);range.memory=upload.memory;range.size=VK_WHOLE_SIZE;
            for(bool bgra:{false,true})for(bool improved:{false,true}){
                const unsigned iterations=bgra&&!improved?2:12;
                double elapsed=0;
                for(unsigned i=0;i<iterations+1;++i){const auto start=std::chrono::steady_clock::now();
                    if(improved)surfxr::rgba_upload_copy(static_cast<std::uint8_t*>(upload.mapped),src.data(),std::size_t{w}*4,w,h,bgra,true);
                    else previous(static_cast<std::uint8_t*>(upload.mapped),src.data(),w,h,bgra);
                    if(!(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))check(vkFlushMappedMemoryRanges(vk.device,1,&range),"flush");
                    if(i>=1)elapsed+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                }
                reference(expected.data(),src.data(),std::size_t{w}*4,w,h,bgra,true);
                require(std::memcmp(upload.mapped,expected.data(),bytes)==0,"mapped_pixel_result_changed");
                if(!first)std::cout<<',';
                first=false;
                std::cout<<"{\"width\":"<<w<<",\"height\":"<<h<<",\"memory_type\":"<<type<<",\"memory_flags\":"<<flags<<",\"bgra\":"<<(bgra?"true":"false")<<",\"improved\":"<<(improved?"true":"false")<<",\"iterations\":"<<iterations<<",\"mean_ms\":"<<elapsed/iterations<<'}';
            }
        }
    }
    std::cout<<"]}\n";
}
using CopyFunction=void(*)(std::uint8_t*,const std::uint8_t*,std::size_t);
void copy_memcpy(std::uint8_t* dst,const std::uint8_t* src,std::size_t bytes){std::memcpy(dst,src,bytes);}
template<std::size_t chunk>void copy_chunks(std::uint8_t* dst,const std::uint8_t* src,std::size_t bytes){
    while(bytes){const auto count=std::min(bytes,chunk);std::memcpy(dst,src,count);dst+=count;src+=count;bytes-=count;}
}
#if defined(__aarch64__)
template<unsigned vectors>void copy_neon(std::uint8_t* dst,const std::uint8_t* src,std::size_t bytes){
    while(bytes>=vectors*16){
        uint8x16_t block[vectors];
        for(unsigned i=0;i<vectors;++i)block[i]=vld1q_u8(src+i*16);
        for(unsigned i=0;i<vectors;++i)vst1q_u8(dst+i*16,block[i]);
        src+=vectors*16;dst+=vectors*16;bytes-=vectors*16;
    }
    if(bytes)std::memcpy(dst,src,bytes);
}
#endif
void vr_copy_benchmark(){
    // Deliberately separate from the desktop feather/BGRA benchmark: current
    // VR sources are tight RGBA and copied into uncached/coherent Vulkan memory.
    // A concurrent XR session competes for bandwidth, so this is only a bounded
    // candidate comparison, never a streaming FPS measurement.
    const std::vector<std::pair<const char*,CopyFunction>> variants{
        {"memcpy_frame",copy_memcpy},{"memcpy_rows_8192",copy_chunks<8192>},{"memcpy_chunks_65536",copy_chunks<65536>},
#if defined(__aarch64__)
        {"neon_64",copy_neon<4>},{"neon_128",copy_neon<8>},{"neon_256",copy_neon<16>},
#endif
    };
    Device vk;VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(vk.physical,&properties);
    constexpr unsigned width=2048,height=2176,iterations=16;
    constexpr std::size_t bytes=std::size_t{width}*height*4;
    std::vector<std::uint8_t> source(bytes);std::uint32_t random=0x73187135;
    for(auto& value:source){random^=random<<13;random^=random>>17;random^=random<<5;value=static_cast<std::uint8_t>(random);}
    std::cout<<"{\"status\":\"benchmark_passed\",\"width\":"<<width<<",\"height\":"<<height
             <<",\"openxr_session_started\":false,\"network_used\":false,\"streaming_fps_measured\":false,\"cases\":[";
    bool first=true;
    for(unsigned type=0;type<properties.memoryTypeCount;++type){
        const auto flags=properties.memoryTypes[type].propertyFlags;if(!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
        Upload upload(vk.device);auto bi=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);bi.size=bytes+128;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(vk.device,&bi,nullptr,&upload.buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(vk.device,upload.buffer,&req);
        if(!(req.memoryTypeBits&(1u<<type)))continue;
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        check(vkAllocateMemory(vk.device,&ai,nullptr,&upload.memory),"allocate");check(vkBindBufferMemory(vk.device,upload.buffer,upload.memory,0),"bind");
        check(vkMapMemory(vk.device,upload.memory,0,VK_WHOLE_SIZE,0,&upload.mapped),"map");
        auto range=info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);range.memory=upload.memory;range.size=VK_WHOLE_SIZE;
        std::vector<std::vector<double>> times(variants.size());
        const auto copy=[&](unsigned variant){
            variants[variant].second(static_cast<std::uint8_t*>(upload.mapped)+64,source.data(),bytes);
            if(!(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))check(vkFlushMappedMemoryRanges(vk.device,1,&range),"flush");
        };
        for(unsigned i=0;i<variants.size();++i){
            std::memset(upload.mapped,0x5a,bytes+128);copy(i);
            require(std::memcmp(static_cast<std::uint8_t*>(upload.mapped)+64,source.data(),bytes)==0,"vr_mapped_copy_changed_pixels");
            const auto* guard=static_cast<const std::uint8_t*>(upload.mapped);
            for(unsigned j=0;j<64;++j)require(guard[j]==0x5a&&guard[bytes+64+j]==0x5a,"vr_mapped_copy_overran_destination");
        }
        for(unsigned iteration=0;iteration<iterations+2;++iteration){
            for(unsigned order=0;order<variants.size();++order){
                const auto variant=iteration%2?variants.size()-1-order:order;
                const auto before=std::chrono::steady_clock::now();copy(static_cast<unsigned>(variant));
                const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
                if(iteration>=2)times[variant].push_back(elapsed);
            }
        }
        for(unsigned i=0;i<variants.size();++i){
            auto& values=times[i];std::sort(values.begin(),values.end());double total=0;for(auto value:values)total+=value;
            if(!first)std::cout<<',';
            first=false;
            std::cout<<"{\"memory_type\":"<<type<<",\"memory_flags\":"<<flags<<",\"variant\":\""<<variants[i].first
                     <<"\",\"byte_identical\":true,\"iterations\":"<<iterations<<",\"mean_ms\":"<<total/values.size()
                     <<",\"median_ms\":"<<(values[7]+values[8])*.5<<",\"min_ms\":"<<values.front()<<",\"max_ms\":"<<values.back()<<'}';
        }
    }
    std::cout<<"]}\n";
}
#endif
}
int main(int argc,char** argv){try{
    if(argc==2&&std::strcmp(argv[1],"--self-test")==0){const auto cases=self_test();std::cout<<"{\"status\":\"self_test_passed\",\"cases\":"<<cases<<",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;}
#ifdef SURFXR_UPLOAD_VULKAN_TEST
    if(argc==2&&std::strcmp(argv[1],"--gpu-direct-test")==0){gpu_direct_test();return 0;}
    if(argc==2&&std::strcmp(argv[1],"--gpu-direct-benchmark")==0){gpu_direct_benchmark();return 0;}
    if(argc==2&&std::strcmp(argv[1],"--benchmark")==0){benchmark();return 0;}
    if(argc==2&&std::strcmp(argv[1],"--vr-copy-benchmark")==0){vr_copy_benchmark();return 0;}
#endif
    std::cout<<"surfxr-rgba-upload-test --self-test | --gpu-direct-test | --gpu-direct-benchmark | --benchmark | --vr-copy-benchmark\n";return argc==1?0:1;
}catch(const std::exception& e){std::cerr<<"rgba_upload_test: "<<e.what()<<'\n';return 1;}}
