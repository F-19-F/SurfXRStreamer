#include "surfxr/image_enhancement_renderer.hpp"
#include "frame/xr/generated/image_enhancement_spirv.hpp"
#include <chrono>
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
struct ImageEnhancementRenderer::Impl {
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    unsigned family{},width{},height{},source_width{},source_height{};VkFormat format{};
    std::array<VkRenderPass,2> passes{};VkDescriptorSetLayout descriptor_layout{};VkPipelineLayout layout{};
    // Key: encoded source bit 0, encoded target bit 1. Upscaling is fixed by
    // this renderer's dimensions; sharpness strength remains a push constant.
    std::array<VkPipeline,4> pipelines{};bool raw_supported=false;
    VkSampler sampler{};VkDescriptorPool descriptors{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    std::map<VkImageView,VkFramebuffer> framebuffers;double wait_ms=0;bool pending=false;
    VkQueryPool timestamps{};unsigned timestamp_bits=0;double timestamp_period=0,gpu_ms=-1;
    VkShaderModule shader(const std::uint32_t* data,std::size_t size){
        auto ci=info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);ci.codeSize=size;ci.pCode=data;
        VkShaderModule value{};check(vkCreateShaderModule(device,&ci,nullptr,&value),"vkCreateShaderModule(image_enhancement)");return value;
    }
    void initialize(){
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
        unsigned queue_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,queues.data());
        if(family<queues.size()&&queues[family].timestampValidBits&&props.limits.timestampPeriod>0){
            timestamp_bits=queues[family].timestampValidBits;timestamp_period=props.limits.timestampPeriod;
            auto qi=info<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
            check(vkCreateQueryPool(device,&qi,nullptr,&timestamps),"vkCreateQueryPool(image_enhancement timestamps)");
        }
        if(!source_width||!source_height||source_width>props.limits.maxImageDimension2D||source_height>props.limits.maxImageDimension2D||
           width<source_width||height<source_height||!width||!height||width>props.limits.maxImageDimension2D||height>props.limits.maxImageDimension2D||
           width>props.limits.maxFramebufferWidth||height>props.limits.maxFramebufferHeight||
           width>props.limits.maxViewportDimensions[0]||height>props.limits.maxViewportDimensions[1])
            throw std::runtime_error("Enhancement target exceeds device limits");
        if(format!=VK_FORMAT_R8G8B8A8_SRGB&&format!=VK_FORMAT_B8G8R8A8_SRGB)throw std::runtime_error("Enhancement requires sRGB image views");
        VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
        constexpr auto required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&required)!=required)throw std::runtime_error("Enhancement image format unsupported");
        const auto raw_format=format==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_B8G8R8A8_UNORM;
        VkFormatProperties raw_properties{};vkGetPhysicalDeviceFormatProperties(physical,raw_format,&raw_properties);
        raw_supported=(raw_properties.optimalTilingFeatures&required)==required;
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
        check(vkCreateRenderPass(device,&rp,nullptr,&passes[0]),"vkCreateRenderPass(image_enhancement)");
        if(raw_supported){at.format=raw_format;check(vkCreateRenderPass(device,&rp,nullptr,&passes[1]),"vkCreateRenderPass(image_enhancement raw)");}
        std::array<VkDescriptorSetLayoutBinding,2> bindings{};
        for(unsigned i=0;i<bindings.size();++i){auto& binding=bindings[i];binding.binding=i;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
        auto dl=info<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);dl.bindingCount=bindings.size();dl.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptor_layout),"vkCreateDescriptorSetLayout(image_enhancement)");
        auto pl=info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);pl.setLayoutCount=1;pl.pSetLayouts=&descriptor_layout;
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"vkCreatePipelineLayout(image_enhancement)");
        VkShaderModule vs{},fs{};
        try{
            vs=shader(image_enhancement_vert_spv,sizeof(image_enhancement_vert_spv));fs=shader(image_enhancement_frag_spv,sizeof(image_enhancement_frag_spv));
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
            const std::array<VkSpecializationMapEntry,3> entries{{
                {0,0,sizeof(VkBool32)},{1,sizeof(VkBool32),sizeof(VkBool32)},
                {2,2*sizeof(VkBool32),sizeof(VkBool32)}}};
            for(unsigned i=0;i<(raw_supported?4u:1u);++i){
                const std::array<VkBool32,3> values{{(i&1)?VK_TRUE:VK_FALSE,(i&2)?VK_TRUE:VK_FALSE,
                    width>source_width||height>source_height?VK_TRUE:VK_FALSE}};
                const VkSpecializationInfo specialization{static_cast<std::uint32_t>(entries.size()),entries.data(),sizeof(values),values.data()};
                stages[1].pSpecializationInfo=&specialization;gp.renderPass=passes[(i&2)?1:0];
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelines[i]),"vkCreateGraphicsPipelines(image_enhancement)");}
        }catch(...){if(vs)vkDestroyShaderModule(device,vs,nullptr);if(fs)vkDestroyShaderModule(device,fs,nullptr);throw;}
        vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
        auto si=info<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);si.magFilter=si.minFilter=VK_FILTER_LINEAR;
        si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device,&si,nullptr,&sampler),"vkCreateSampler(image_enhancement)");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4};
        auto dp=info<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);dp.maxSets=2;dp.poolSizeCount=1;dp.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors),"vkCreateDescriptorPool(image_enhancement)");
        auto pc=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&pc,nullptr,&pool),"vkCreateCommandPool(image_enhancement)");
        auto ca=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&command),"vkAllocateCommandBuffers(image_enhancement)");
        auto fi=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);check(vkCreateFence(device,&fi,nullptr,&fence),"vkCreateFence(image_enhancement)");
    }
    VkFramebuffer framebuffer(VkImageView view,bool raw){
        const auto found=framebuffers.find(view);if(found!=framebuffers.end())return found->second;
        auto ci=info<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);ci.renderPass=passes[raw?1:0];ci.attachmentCount=1;ci.pAttachments=&view;
        ci.width=width;ci.height=height;ci.layers=1;VkFramebuffer value{};
        check(vkCreateFramebuffer(device,&ci,nullptr,&value),"vkCreateFramebuffer(image_enhancement)");framebuffers.emplace(view,value);return value;
    }
    void complete(bool covered){
        if(!pending)return;
        if(!covered||vkGetFenceStatus(device,fence)!=VK_SUCCESS){
            const auto before=std::chrono::steady_clock::now();
            const auto result=vkWaitForFences(device,1,&fence,VK_TRUE,2'000'000'000ULL);
            wait_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            if(result!=VK_SUCCESS){vkDeviceWaitIdle(device);check(result,"vkWaitForFences(image_enhancement,2s)");}
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
    bool render(const std::array<ImageEnhancementEye,2>& eyes,ImageEnhancementOptions options,bool defer_wait){
        complete(false);
        wait_ms=0;gpu_ms=-1;
        if(!valid_image_enhancement_options(options))throw std::runtime_error("Invalid enhancement options");
        const bool upscale=options.super_resolution&&(width>source_width||height>source_height);
        if(!upscale&&options.sharpness==0)return false;
        if(!options.super_resolution&&(width!=source_width||height!=source_height))throw std::runtime_error("Sharpen-only target must match complete eye");
        if(!eyes[0].dirty&&!eyes[1].dirty)return false;
        for(const auto& eye:eyes)if(eye.dirty&&(!eye.source||!eye.source_view||!eye.target||eye.source_view==eye.target))
            throw std::runtime_error("Invalid image_enhancement image");
        for(const auto& eye:eyes)if(eye.dirty&&((eye.raw_source_view||eye.raw_target)&&!raw_supported))
            throw std::runtime_error("Raw enhancement views unsupported");
        for(const auto& eye:eyes)if(eye.dirty&&eye.raw_target&&(eye.raw_target==eye.source_view||eye.raw_target==eye.raw_source_view))
            throw std::runtime_error("Overlapping raw enhancement views");
        if(eyes[0].dirty&&eyes[1].dirty&&(eyes[0].source==eyes[1].source||eyes[0].target==eyes[1].target))
            throw std::runtime_error("Duplicate image_enhancement image");
        if(eyes[0].dirty&&eyes[1].dirty&&eyes[0].raw_target&&eyes[0].raw_target==eyes[1].raw_target)
            throw std::runtime_error("Duplicate raw enhancement target");
        check(vkResetDescriptorPool(device,descriptors,0),"vkResetDescriptorPool(image_enhancement)");
        const std::array<VkDescriptorSetLayout,2> layouts{{descriptor_layout,descriptor_layout}};std::array<VkDescriptorSet,2> sets{};
        auto ai=info<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);ai.descriptorPool=descriptors;ai.descriptorSetCount=2;ai.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device,&ai,sets.data()),"vkAllocateDescriptorSets(image_enhancement)");
        std::array<VkFramebuffer,2> fb{};
        for(unsigned e=0;e<2;++e)if(eyes[e].dirty){const auto& eye=eyes[e];fb[e]=framebuffer(eye.raw_target?eye.raw_target:eye.target,eye.raw_target!=VK_NULL_HANDLE);
            const std::array<VkDescriptorImageInfo,2> images{{{sampler,eye.source_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler,eye.raw_source_view?eye.raw_source_view:eye.source_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
            std::array<VkWriteDescriptorSet,2> writes{};
            for(unsigned binding=0;binding<writes.size();++binding){auto& write=writes[binding];write=info<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                write.dstSet=sets[e];write.dstBinding=binding;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&images[binding];}
            vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);}
        check(vkResetCommandBuffer(command,0),"vkResetCommandBuffer(image_enhancement)");check(vkResetFences(device,1,&fence),"vkResetFences(image_enhancement)");
        auto begin=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&begin),"vkBeginCommandBuffer(image_enhancement)");
        if(timestamps)vkCmdResetQueryPool(command,timestamps,0,2);
        const auto transition=[&](bool entering){
            std::array<VkImageMemoryBarrier,2> barriers{};unsigned count=0;
            for(const auto& eye:eyes)if(eye.dirty){auto& barrier=barriers[count++];barrier=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=eye.source;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                barrier.oldLayout=entering?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout=entering?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask=entering?(VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT):VK_ACCESS_SHADER_READ_BIT;
                barrier.dstAccessMask=entering?VK_ACCESS_SHADER_READ_BIT:(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);}
            vkCmdPipelineBarrier(command,entering?(VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT):VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                entering?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,count,barriers.data());
        };
        transition(true);
        // Timestamping makes the already-required source dependency a full
        // execution boundary: no enhancement stages may precede the start
        // timestamp. This allows its span to be added to foveation's span,
        // at the cost of also serializing the tiny vertex/setup work. It adds
        // no CPU wait and no extra memory dependency.
        if(timestamps){
            vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,0);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                0,0,nullptr,0,nullptr,0,nullptr);
        }
        for(unsigned e=0;e<2;++e)if(eyes[e].dirty){
            const auto raw=eyes[e].raw_target!=VK_NULL_HANDLE;
            const unsigned variant=(eyes[e].raw_source_view?1u:0u)|(raw?2u:0u);
            const std::array<float,8> constants{{1.f/source_width,1.f/source_height,float(source_width),float(source_height),0,options.sharpness,0,0}};
            auto rp=info<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);rp.renderPass=passes[raw?1:0];rp.framebuffer=fb[e];rp.renderArea.extent={width,height};
            vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[variant]);
            const VkViewport viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};const VkRect2D scissor{{0,0},{width,height}};
            vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[e],0,nullptr);
            vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),constants.data());vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        }
        transition(false);
        if(timestamps)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,1);
        check(vkEndCommandBuffer(command),"vkEndCommandBuffer(image_enhancement)");
        auto submit=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        check(vkQueueSubmit(queue,1,&submit,fence),"vkQueueSubmit(image_enhancement)");pending=true;
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
ImageEnhancementRenderer::ImageEnhancementRenderer(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,VkFormat format,unsigned source_width,unsigned source_height,unsigned width,unsigned height)
    :impl_(std::make_unique<Impl>()){
    impl_->physical=physical;impl_->device=device;impl_->queue=queue;impl_->family=family;impl_->format=format;impl_->width=width;impl_->height=height;impl_->source_width=source_width;impl_->source_height=source_height;impl_->initialize();
}
ImageEnhancementRenderer::~ImageEnhancementRenderer()=default;
bool ImageEnhancementRenderer::render(const std::array<ImageEnhancementEye,2>& eyes,ImageEnhancementOptions options,bool defer_wait){return impl_->render(eyes,options,defer_wait);}
void ImageEnhancementRenderer::complete(bool covered){impl_->complete(covered);}
double ImageEnhancementRenderer::last_gpu_wait_ms()const noexcept{return impl_->wait_ms;}
double ImageEnhancementRenderer::last_gpu_duration_ms()const noexcept{return impl_->gpu_ms;}
bool ImageEnhancementRenderer::supports_gpu_timestamps()const noexcept{return impl_->timestamps!=VK_NULL_HANDLE;}
bool ImageEnhancementRenderer::supports_raw_views()const noexcept{return impl_->raw_supported;}
} // namespace surfxr
