#include "surfxr/foveation_renderer.hpp"
#include "frame/xr/generated/foveation_spirv.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace surfxr {
namespace {
template<class T>T info(VkStructureType type){T value{};value.sType=type;return value;}
void check(VkResult result,const char* call){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(call)+": VkResult "+std::to_string(result));}
}
struct FoveationRenderer::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    unsigned family{},width{},height{};VkFormat format{};
    std::array<VkRenderPass,2> passes{};VkDescriptorSetLayout descriptor_layout{};VkPipelineLayout layout{};
    // Original reconstruction, fused sharpen to sRGB, fused sharpen to UNORM.
    std::array<VkPipeline,3> pipelines{};bool srgb_format=false,raw_supported=false,allow_quad=false,quad_supported=false,zero_weight_skip=true;
    FoveationSharpening sharpening=FoveationSharpening::EncodedSrgb8;
    VkSampler sampler{};VkDescriptorPool descriptors{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    std::array<VkDescriptorSet,2> sets{};
    std::map<VkImageView,VkFramebuffer> framebuffers;double wait_ms=0;bool pending=false;
    VkQueryPool timestamps{};unsigned timestamp_bits=0;double timestamp_period=0,gpu_ms=-1;
    VkShaderModule shader(const std::uint32_t* data,std::size_t size){
        auto ci=info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);ci.codeSize=size;ci.pCode=data;
        VkShaderModule value{};check(vkCreateShaderModule(device,&ci,nullptr,&value),"vkCreateShaderModule(foveation)");return value;
    }
    void initialize(){
        if(sharpening!=FoveationSharpening::EncodedSrgb8&&sharpening!=FoveationSharpening::Gamma2)
            throw std::runtime_error("Invalid foveation sharpening domain");
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
        if(allow_quad&&props.apiVersion>=VK_API_VERSION_1_1){
            auto subgroup=info<VkPhysicalDeviceSubgroupProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
            auto properties=info<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);properties.pNext=&subgroup;
            vkGetPhysicalDeviceProperties2(physical,&properties);
            constexpr auto operations=VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_QUAD_BIT;
            quad_supported=subgroup.subgroupSize>=4&&(subgroup.supportedStages&VK_SHADER_STAGE_FRAGMENT_BIT)
                &&(subgroup.supportedOperations&operations)==operations;
        }
        unsigned queue_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,queues.data());
        if(family<queues.size()&&queues[family].timestampValidBits&&props.limits.timestampPeriod>0){
            timestamp_bits=queues[family].timestampValidBits;timestamp_period=props.limits.timestampPeriod;
            auto qi=info<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
            check(vkCreateQueryPool(device,&qi,nullptr,&timestamps),"vkCreateQueryPool(foveation timestamps)");
        }
        if(!width||!height||width>props.limits.maxImageDimension2D||height>props.limits.maxImageDimension2D||
           width>props.limits.maxFramebufferWidth||height>props.limits.maxFramebufferHeight||
           width>props.limits.maxViewportDimensions[0]||height>props.limits.maxViewportDimensions[1])
            throw std::runtime_error("Foveation target exceeds device limits");
        VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
        constexpr auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&required)!=required)throw std::runtime_error("Foveation image format unsupported");
        srgb_format=format==VK_FORMAT_R8G8B8A8_SRGB||format==VK_FORMAT_B8G8R8A8_SRGB;
        const auto raw_format=format==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_B8G8R8A8_UNORM;
        if(srgb_format&&sharpening==FoveationSharpening::EncodedSrgb8){VkFormatProperties raw_properties{};vkGetPhysicalDeviceFormatProperties(physical,raw_format,&raw_properties);
            raw_supported=(raw_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)!=0;}
        VkAttachmentDescription at{};at.format=format;at.samples=VK_SAMPLE_COUNT_1_BIT;
        at.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout=at.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
        std::array<VkSubpassDependency,2> dependencies{};
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
        dependencies[0].srcStageMask=dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask=dependencies[1].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        auto rp=info<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);rp.attachmentCount=1;rp.pAttachments=&at;
        rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=dependencies.data();
        check(vkCreateRenderPass(device,&rp,nullptr,&passes[0]),"vkCreateRenderPass(foveation)");
        if(raw_supported){at.format=raw_format;check(vkCreateRenderPass(device,&rp,nullptr,&passes[1]),"vkCreateRenderPass(foveation raw)");}
        VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        std::array<VkDescriptorSetLayoutBinding,3> bindings{{binding,binding,binding}};bindings[1].binding=1;bindings[2].binding=2;
        auto dl=info<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);dl.bindingCount=3;dl.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptor_layout),"vkCreateDescriptorSetLayout(foveation)");
        auto pl=info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);pl.setLayoutCount=1;pl.pSetLayouts=&descriptor_layout;
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,128};pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"vkCreatePipelineLayout(foveation)");
        VkShaderModule vs{},fs{},quad_fs{};
        try{
            vs=shader(foveation_vert_spv,sizeof(foveation_vert_spv));fs=shader(foveation_frag_spv,sizeof(foveation_frag_spv));
            if(quad_supported)quad_fs=shader(foveation_quad_frag_spv,sizeof(foveation_quad_frag_spv));
            std::array<VkPipelineShaderStageCreateInfo,2> stages{};
            for(auto& stage:stages)stage=info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
            stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
            auto vi=info<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
            auto ia=info<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            auto vp=info<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);vp.viewportCount=vp.scissorCount=1;
            auto rs=info<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
            rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
            auto ms=info<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState color_blend{};color_blend.colorWriteMask=15;
            auto bs=info<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);bs.attachmentCount=1;bs.pAttachments=&color_blend;
            const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            auto dynamic=info<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
            auto gp=info<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);gp.stageCount=2;gp.pStages=stages.data();
            gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;
            gp.pMultisampleState=&ms;gp.pColorBlendState=&bs;gp.pDynamicState=&dynamic;gp.layout=layout;
            const std::array<VkSpecializationMapEntry,4> entries{{{0,0,sizeof(VkBool32)},{1,sizeof(VkBool32),sizeof(VkBool32)},
                {2,2*sizeof(VkBool32),sizeof(VkBool32)},{3,3*sizeof(VkBool32),sizeof(VkBool32)}}};
            for(unsigned i=0;i<(raw_supported?3u:srgb_format?2u:1u);++i){
                const std::array<VkBool32,4> values{{VkBool32(i!=0),VkBool32(i==2),VkBool32(zero_weight_skip),
                    VkBool32(sharpening==FoveationSharpening::Gamma2)}};
                const VkSpecializationInfo specialization{static_cast<std::uint32_t>(entries.size()),entries.data(),sizeof(values),values.data()};
                stages[1].pSpecializationInfo=&specialization;stages[1].module=i&&quad_fs?quad_fs:fs;gp.renderPass=passes[i==2?1:0];
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelines[i]),"vkCreateGraphicsPipelines(foveation)");}
        }catch(...){if(vs)vkDestroyShaderModule(device,vs,nullptr);if(fs)vkDestroyShaderModule(device,fs,nullptr);if(quad_fs)vkDestroyShaderModule(device,quad_fs,nullptr);throw;}
        vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
        if(quad_fs)vkDestroyShaderModule(device,quad_fs,nullptr);
        auto si=info<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);si.magFilter=si.minFilter=VK_FILTER_LINEAR;
        si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device,&si,nullptr,&sampler),"vkCreateSampler(foveation)");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6};
        auto dp=info<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);dp.maxSets=2;dp.poolSizeCount=1;dp.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors),"vkCreateDescriptorPool(foveation)");
        const std::array<VkDescriptorSetLayout,2> layouts{{descriptor_layout,descriptor_layout}};
        auto da=info<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);da.descriptorPool=descriptors;da.descriptorSetCount=2;da.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device,&da,sets.data()),"vkAllocateDescriptorSets(foveation)");
        auto pc=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&pc,nullptr,&pool),"vkCreateCommandPool(foveation)");
        auto ca=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&command),"vkAllocateCommandBuffers(foveation)");
        auto fi=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);check(vkCreateFence(device,&fi,nullptr,&fence),"vkCreateFence(foveation)");
    }
    VkFramebuffer framebuffer(VkImageView view,bool raw){
        const auto found=framebuffers.find(view);if(found!=framebuffers.end())return found->second;
        auto ci=info<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);ci.renderPass=passes[raw?1:0];ci.attachmentCount=1;ci.pAttachments=&view;
        ci.width=width;ci.height=height;ci.layers=1;VkFramebuffer value{};
        check(vkCreateFramebuffer(device,&ci,nullptr,&value),"vkCreateFramebuffer(foveation)");framebuffers.emplace(view,value);return value;
    }
    void complete(bool covered){
        if(!pending)return;
        if(!covered||vkGetFenceStatus(device,fence)!=VK_SUCCESS){
            const auto before=std::chrono::steady_clock::now();
            const auto result=vkWaitForFences(device,1,&fence,VK_TRUE,2'000'000'000ULL);
            wait_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            if(result!=VK_SUCCESS){vkDeviceWaitIdle(device);check(result,"vkWaitForFences(foveation,2s)");}
        }
        if(timestamps){
            std::array<std::uint64_t,2> values{};
            if(vkGetQueryPoolResults(device,timestamps,0,2,sizeof(values),values.data(),sizeof(values[0]),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
                const auto mask=timestamp_bits>=64?UINT64_MAX:(std::uint64_t{1}<<timestamp_bits)-1;
                gpu_ms=double((values[1]-values[0])&mask)*timestamp_period/1'000'000.;
            }
        }
        pending=false;
    }
    bool render(const std::array<FoveationEye,2>& eyes,bool defer_wait,float sharpness){
        complete(false);
        wait_ms=0;gpu_ms=-1;
        if(!std::isfinite(sharpness)||sharpness<0||sharpness>1||(sharpness>0&&!srgb_format))
            throw std::runtime_error("Invalid fused foveation sharpness or format");
        if(sharpness>0&&sharpening==FoveationSharpening::Gamma2)
            for(const auto& eye:eyes)if(eye.raw_target)throw std::runtime_error("Gamma2 sharpening requires an sRGB target view");
        if(!eyes[0].dirty&&!eyes[1].dirty)return false;
        for(const auto& eye:eyes)if(eye.dirty&&(!eye.source||!eye.source_view||!eye.target||eye.source_view==eye.target))
            throw std::runtime_error("Invalid foveation image");
        for(const auto& eye:eyes)if(eye.dirty&&(!std::all_of(eye.source_rect.begin(),eye.source_rect.end(),[](float v){return std::isfinite(v);})||
            eye.source_rect[0]<0||eye.source_rect[1]<0||eye.source_rect[2]<=0||eye.source_rect[3]<=0||
            eye.source_rect[0]+eye.source_rect[2]>1||eye.source_rect[1]+eye.source_rect[3]>1))
            throw std::runtime_error("Invalid foveation source region");
        for(const auto& eye:eyes)if(eye.dirty&&eye.chroma_view&&(!eye.chroma_source||(eye.chroma_source==eye.source&&!eye.foreign_source)||
            !eye.host_source||
            !std::all_of(eye.yuv_coefficients.begin(),eye.yuv_coefficients.end(),[](float v){return std::isfinite(v);})||
            eye.yuv_coefficients[0]<=0||eye.yuv_coefficients[1]<=0||eye.yuv_coefficients[0]+eye.yuv_coefficients[1]>=1||
            eye.yuv_coefficients[2]<0||eye.yuv_coefficients[2]>.1f||eye.yuv_coefficients[3]<1||eye.yuv_coefficients[3]>1.2f))
            throw std::runtime_error("Invalid NV12 reconstruction source");
        for(const auto& eye:eyes)if(eye.dirty&&eye.focus_view){
            if(!eye.focus_source||(eye.focus_source==eye.source&&(!eye.host_source||!eye.focus_host_source||eye.host_source_initialized!=eye.focus_host_initialized))||eye.focus_view==eye.target||
               !std::all_of(eye.focus_source_rect.begin(),eye.focus_source_rect.end(),[](float v){return std::isfinite(v);})||
               eye.focus_source_rect[0]<0||eye.focus_source_rect[1]<0||eye.focus_source_rect[2]<=0||eye.focus_source_rect[3]<=0||
               eye.focus_source_rect[0]+eye.focus_source_rect[2]>1||eye.focus_source_rect[1]+eye.focus_source_rect[3]>1||
               !std::all_of(eye.focus_rect.begin(),eye.focus_rect.end(),[](float v){return std::isfinite(v);})||
               eye.focus_rect[2]<=0||eye.focus_rect[3]<=0||eye.focus_rect[2]>width||eye.focus_rect[3]>height)
                throw std::runtime_error("Invalid focus crop");
        }
        if(eyes[0].dirty&&eyes[1].dirty&&(eyes[0].target==eyes[1].target||
           (eyes[0].source==eyes[1].source&&(!eyes[0].host_source||!eyes[1].host_source||
             eyes[0].host_source_initialized!=eyes[1].host_source_initialized))))
            throw std::runtime_error("Duplicate foveation image");
        if(sharpness>0){
            for(const auto& eye:eyes)if(eye.dirty&&eye.raw_target&&(!raw_supported||eye.raw_target==eye.source_view))
                throw std::runtime_error("Invalid raw fused foveation target");
            if(eyes[0].dirty&&eyes[1].dirty&&eyes[0].raw_target&&eyes[0].raw_target==eyes[1].raw_target)
                throw std::runtime_error("Duplicate raw fused foveation target");
        }
        // complete() above makes the two persistent descriptor sets writable.
        std::array<VkFramebuffer,2> fb{};
        for(unsigned e=0;e<2;++e)if(eyes[e].dirty){const auto raw=sharpness>0&&eyes[e].raw_target;
            fb[e]=framebuffer(raw?eyes[e].raw_target:eyes[e].target,raw);
            VkDescriptorImageInfo image{sampler,eyes[e].source_view,eyes[e].host_source?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto write=info<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);write.dstSet=sets[e];write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            VkDescriptorImageInfo focus{sampler,eyes[e].focus_view?eyes[e].focus_view:eyes[e].source_view,
                (eyes[e].focus_view?eyes[e].focus_host_source:eyes[e].host_source)?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            write.dstBinding=1;write.pImageInfo=&focus;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            VkDescriptorImageInfo chroma{sampler,eyes[e].chroma_view?eyes[e].chroma_view:eyes[e].source_view,
                eyes[e].host_source?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            write.dstBinding=2;write.pImageInfo=&chroma;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
        check(vkResetCommandBuffer(command,0),"vkResetCommandBuffer(foveation)");check(vkResetFences(device,1,&fence),"vkResetFences(foveation)");
        auto begin=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&begin),"vkBeginCommandBuffer(foveation)");
        if(timestamps){vkCmdResetQueryPool(command,timestamps,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,0);}
        const auto transition=[&](bool entering){
            std::array<VkImageMemoryBarrier,6> barriers{};unsigned count=0;
            VkPipelineStageFlags before=0,after=0;
            const auto add=[&](VkImage image,bool host,bool initialized,bool foreign){
                if(host&&!entering&&!foreign)return;
                for(unsigned i=0;i<count;++i)if(barriers[i].image==image)return;
                auto& barrier=barriers[count++];barrier=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                if(foreign){
                    barrier.srcQueueFamilyIndex=entering?VK_QUEUE_FAMILY_FOREIGN_EXT:family;
                    barrier.dstQueueFamilyIndex=entering?family:VK_QUEUE_FAMILY_FOREIGN_EXT;
                    barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                    barrier.srcAccessMask=entering?0:VK_ACCESS_SHADER_READ_BIT;
                    barrier.dstAccessMask=entering?VK_ACCESS_SHADER_READ_BIT:0;
                    before|=entering?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    after|=entering?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;return;
                }
                if(host){
                    barrier.oldLayout=initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PREINITIALIZED;
                    barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                    before|=VK_PIPELINE_STAGE_HOST_BIT;after|=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;return;
                }
                barrier.oldLayout=entering?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout=entering?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask=entering?(VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT):VK_ACCESS_SHADER_READ_BIT;
                barrier.dstAccessMask=entering?VK_ACCESS_SHADER_READ_BIT:(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                before|=entering?(VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT):VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                after|=entering?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            };
            for(const auto& eye:eyes)if(eye.dirty){
                add(eye.source,eye.host_source,eye.host_source_initialized,eye.foreign_source);
                if(eye.chroma_view)add(eye.chroma_source,true,eye.host_source_initialized,eye.foreign_source);
                if(eye.focus_view)add(eye.focus_source,eye.focus_host_source,eye.focus_host_initialized,eye.foreign_source&&eye.focus_source==eye.source);
            }
            if(count)vkCmdPipelineBarrier(command,before,after,0,0,nullptr,0,nullptr,count,barriers.data());
        };
        transition(true);
        for(unsigned e=0;e<2;++e)if(eyes[e].dirty){
            const auto raw=sharpness>0&&eyes[e].raw_target;
            const unsigned variant=sharpness>0?(raw?2u:1u):0u;
            const std::array<float,32> constants{{1.f/width,1.f/height,float(width),float(height),eyes[e].desktop_border?1.f:0.f,sharpness,eyes[e].focus_view?1.f:0.f,eyes[e].linear_layout?1.f:0.f,
                eyes[e].focus_rect[0],eyes[e].focus_rect[1],eyes[e].focus_rect[2],eyes[e].focus_rect[3],
                eyes[e].source_rect[0],eyes[e].source_rect[1],eyes[e].source_rect[2],eyes[e].source_rect[3],
                eyes[e].chroma_view?eyes[e].yuv_coefficients[0]:0,eyes[e].yuv_coefficients[1],eyes[e].yuv_coefficients[2],eyes[e].yuv_coefficients[3],
                eyes[e].focus_source_rect[0],eyes[e].focus_source_rect[1],eyes[e].focus_source_rect[2],eyes[e].focus_source_rect[3],
                eyes[e].timewarp_rotation[0],eyes[e].timewarp_rotation[1],eyes[e].timewarp_rotation[2],eyes[e].timewarp_rotation[3],
                eyes[e].timewarp_fov[0],eyes[e].timewarp_fov[1],eyes[e].timewarp_fov[2],eyes[e].timewarp_fov[3]}};
            auto rp=info<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);rp.renderPass=passes[raw?1:0];rp.framebuffer=fb[e];rp.renderArea.extent={width,height};
            vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[variant]);
            const VkViewport viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};const VkRect2D scissor{{0,0},{width,height}};
            vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[e],0,nullptr);
            vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),constants.data());
            vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        }
        transition(false);
        if(timestamps)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,1);
        check(vkEndCommandBuffer(command),"vkEndCommandBuffer(foveation)");
        auto submit=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        check(vkQueueSubmit(queue,1,&submit,fence),"vkQueueSubmit(foveation)");pending=true;
        if(!defer_wait)complete(false);
        return true;
    }
    ~Impl(){
        if(!device)return;
        vkDeviceWaitIdle(device);
        for(const auto& pair:framebuffers)vkDestroyFramebuffer(device,pair.second,nullptr);
        for(auto pipeline:pipelines)if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(descriptor_layout)vkDestroyDescriptorSetLayout(device,descriptor_layout,nullptr);
        if(sampler)vkDestroySampler(device,sampler,nullptr);
        for(auto pass:passes)if(pass)vkDestroyRenderPass(device,pass,nullptr);
        if(timestamps)vkDestroyQueryPool(device,timestamps,nullptr);
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
    }
};
FoveationRenderer::FoveationRenderer(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,VkFormat format,unsigned width,unsigned height,bool allow_quad,bool zero_weight_skip,FoveationSharpening sharpening)
    :impl_(std::make_unique<Impl>()){
    impl_->physical=physical;impl_->device=device;impl_->queue=queue;impl_->family=family;impl_->format=format;impl_->width=width;impl_->height=height;
    impl_->allow_quad=allow_quad;impl_->zero_weight_skip=zero_weight_skip;impl_->sharpening=sharpening;impl_->initialize();
}
FoveationRenderer::~FoveationRenderer()=default;
bool FoveationRenderer::render(const std::array<FoveationEye,2>& eyes,bool defer_wait,float sharpness){return impl_->render(eyes,defer_wait,sharpness);}
void FoveationRenderer::complete(bool covered){impl_->complete(covered);}
double FoveationRenderer::last_gpu_wait_ms()const noexcept{return impl_->wait_ms;}
double FoveationRenderer::last_gpu_duration_ms()const noexcept{return impl_->gpu_ms;}
bool FoveationRenderer::supports_gpu_timestamps()const noexcept{return impl_->timestamps!=VK_NULL_HANDLE;}
bool FoveationRenderer::supports_raw_views()const noexcept{return impl_->raw_supported;}
bool FoveationRenderer::supports_quad_sharpening()const noexcept{return impl_->quad_supported;}
bool FoveationRenderer::zero_weight_skip_enabled()const noexcept{return impl_->zero_weight_skip;}
} // namespace surfxr
