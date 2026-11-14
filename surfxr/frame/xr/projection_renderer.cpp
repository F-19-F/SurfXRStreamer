// Vulkan curved desktop mesh, independent of OpenXR session ownership.
#include "projection_private.hpp"
#include "frame/xr/generated/desktop_mesh_spirv.hpp"

namespace surfxr {
using namespace projection_detail;
std::array<float,16> projection_view_matrix(UiPose eye,const std::array<float,4>& fov,float n,float f){
    for(float a:fov)if(!std::isfinite(a)||std::abs(a)>=1.5707f)throw std::runtime_error("Invalid projection FOV");
    if(!(n>0&&f>n&&std::isfinite(f)))throw std::runtime_error("Invalid projection depth range");
    const float l=std::tan(fov[0]),r=std::tan(fov[1]),u=std::tan(fov[2]),d=std::tan(fov[3]);
    if(!(r>l&&u>d))throw std::runtime_error("Inverted projection FOV");
    std::array<float,16> p{};
    p[0]=2/(r-l);p[5]=2/(d-u);p[8]=(r+l)/(r-l);p[9]=(u+d)/(d-u);
    p[10]=-f/(f-n);p[11]=-1;p[14]=-f*n/(f-n);
    const auto inv=ui_inverse(eye);const auto x=ui_rotate(inv.orientation,{1,0,0});
    const auto y=ui_rotate(inv.orientation,{0,1,0}),z=ui_rotate(inv.orientation,{0,0,1});
    const std::array<float,16> v{{x.x,x.y,x.z,0,y.x,y.y,y.z,0,z.x,z.y,z.z,0,
        inv.position.x,inv.position.y,inv.position.z,1}};
    std::array<float,16> result{};
    for(unsigned c=0;c<4;++c)for(unsigned row=0;row<4;++row)for(unsigned k=0;k<4;++k)result[c*4+row]+=p[k*4+row]*v[c*4+k];
    return result;
}
std::vector<ProjectionVertex> projection_panel_mesh(const UiPanel& p,unsigned count){
    if(count<1||count>64||!std::isfinite(p.width)||!std::isfinite(p.height)||p.width<=0||p.height<=0||
       !std::isfinite(p.curvature_radians)||p.curvature_radians<0||p.curvature_radians>1.8f)
        throw std::runtime_error("Invalid projection mesh geometry");
    std::vector<ProjectionVertex> out;out.reserve(count*6);
    const auto vertex=[&](float u,float v){
        const auto q=ui_panel_point(p,u,v);const float theta=(u-.5f)*p.curvature_radians;
        const auto n=ui_rotate(p.pose.orientation,{-std::sin(theta),0,std::cos(theta)});
        out.push_back({{q.x,q.y,q.z},{u,v},{n.x,n.y,n.z}});
    };
    for(unsigned i=0;i<count;++i){const float a=static_cast<float>(i)/count,b=static_cast<float>(i+1)/count;
        vertex(a,0);vertex(a,1);vertex(b,0);vertex(b,0);vertex(a,1);vertex(b,1);}
    return out;
}
unsigned ProjectionRenderer::Impl::memory_type(unsigned bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred){
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(physical,&props);unsigned found=UINT32_MAX;
        for(unsigned i=0;i<props.memoryTypeCount;++i)if((bits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&required)==required){
            found=i;
        if((props.memoryTypes[i].propertyFlags&preferred)==preferred)break;}
        if(found==UINT32_MAX)throw std::runtime_error("Projection Vulkan memory type unavailable");
        return found;
    }

void ProjectionRenderer::Impl::image(Image& target,VkFormat fmt,VkImageUsageFlags usage,VkImageAspectFlags aspect){
        auto ci=info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);ci.imageType=VK_IMAGE_TYPE_2D;ci.format=fmt;
        ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=samples;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=usage|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device,&ci,nullptr,&target.image),"vkCreateImage(projection)");
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,target.image,&req);
        auto ai=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ai.allocationSize=req.size;
        ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&target.memory),"vkAllocateMemory(projection image)");
        check(vkBindImageMemory(device,target.image,target.memory,0),"vkBindImageMemory(projection)");
        auto vi=info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);vi.image=target.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=fmt;vi.subresourceRange={aspect,0,1,0,1};check(vkCreateImageView(device,&vi,nullptr,&target.view),"vkCreateImageView(projection)");
    }

VkShaderModule ProjectionRenderer::Impl::shader(const std::uint32_t* data,std::size_t size){auto ci=info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        ci.codeSize=size;ci.pCode=data;VkShaderModule result{};check(vkCreateShaderModule(device,&ci,nullptr,&result),"vkCreateShaderModule(mesh)");return result;}

void ProjectionRenderer::Impl::initialize(){
        if(max_samples!=1&&max_samples!=2&&max_samples!=4)throw std::runtime_error("Projection max_samples must be 1, 2 or 4");
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
        unsigned queue_count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queue_count,queues.data());
        if(timestamp_interval&&family<queues.size()&&queues[family].timestampValidBits&&props.limits.timestampPeriod>0){
            auto queries=info<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);queries.queryType=VK_QUERY_TYPE_TIMESTAMP;queries.queryCount=3;
            if(vkCreateQueryPool(device,&queries,nullptr,&timestamps)==VK_SUCCESS){
                timestamp_bits=queues[family].timestampValidBits;timestamp_tick_ns=props.limits.timestampPeriod;stats.gpu_timestamps=true;
            }else timestamps=VK_NULL_HANDLE;
        }
        if(!width||!height||width>props.limits.maxImageDimension2D||height>props.limits.maxImageDimension2D||
           width>props.limits.maxFramebufferWidth||height>props.limits.maxFramebufferHeight)throw std::runtime_error("Projection size exceeds device limits");
        VkImageFormatProperties cp{},dp{};
        check(vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,0,&cp),"vkGetPhysicalDeviceImageFormatProperties(color)");
        bool depth_found=false;
        for(auto candidate:{VK_FORMAT_D32_SFLOAT,VK_FORMAT_D16_UNORM}){
            if(vkGetPhysicalDeviceImageFormatProperties(physical,candidate,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,0,&dp)==VK_SUCCESS){depth_format=candidate;depth_found=true;break;}}
        if(!depth_found)throw std::runtime_error("Projection depth format unavailable");
        const auto counts=cp.sampleCounts&dp.sampleCounts&props.limits.framebufferColorSampleCounts&props.limits.framebufferDepthSampleCounts;
        for(auto candidate:{VK_SAMPLE_COUNT_4_BIT,VK_SAMPLE_COUNT_2_BIT,VK_SAMPLE_COUNT_1_BIT})if((counts&candidate)&&static_cast<unsigned>(candidate)<=max_samples){samples=candidate;break;}
        if(samples!=VK_SAMPLE_COUNT_1_BIT)image(color,format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        image(depth,depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        std::array<VkAttachmentDescription,3> at{};
        at[0].format=format;at[0].samples=samples;at[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        at[0].storeOp=samples==VK_SAMPLE_COUNT_1_BIT?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at[1].format=depth_format;at[1].samples=samples;at[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;at[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        at[2]=at[0];at[2].samples=VK_SAMPLE_COUNT_1_BIT;at[2].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at[2].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkAttachmentReference cr{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},dr{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},rr{2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&cr;sub.pDepthStencilAttachment=&dr;
        if(samples!=VK_SAMPLE_COUNT_1_BIT)sub.pResolveAttachments=&rr;
        // The same transient color/depth attachments are reused sequentially for
        // both eyes. Order their previous writes before the next clear/resolve.
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        auto rp=info<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);rp.attachmentCount=samples==VK_SAMPLE_COUNT_1_BIT?2:3;rp.pAttachments=at.data();
        rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dep;check(vkCreateRenderPass(device,&rp,nullptr,&pass),"vkCreateRenderPass(mesh)");
        VkFormatProperties source_features{};vkGetPhysicalDeviceFormatProperties(physical,format,&source_features);
        constexpr VkFormatFeatureFlags mip_features=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
        quality_supported=(format==VK_FORMAT_R8G8B8A8_SRGB||format==VK_FORMAT_B8G8R8A8_SRGB)&&
            (source_features.optimalTilingFeatures&mip_features)==mip_features;
        if(quality_supported){
            VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference target{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription copy{};
            copy.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;copy.colorAttachmentCount=1;copy.pColorAttachments=&target;
            auto create=info<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
            create.attachmentCount=1;create.pAttachments=&attachment;create.subpassCount=1;create.pSubpasses=&copy;
            check(vkCreateRenderPass(device,&create,nullptr,&mip_pass),"vkCreateRenderPass(mip0)");
            raw_format=format==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_B8G8R8A8_UNORM;
            attachment.format=raw_format;
            if(image_format_list)check(vkCreateRenderPass(device,&create,nullptr,&nv12_mip_pass),"vkCreateRenderPass(NV12 mip0)");
        }
        VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        auto dl=info<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);std::array<VkDescriptorSetLayoutBinding,2> bindings{binding,binding};bindings[1].binding=1;
        dl.bindingCount=2;dl.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptor_layout),"vkCreateDescriptorSetLayout(mesh)");
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Camera)};auto pl=info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pl.setLayoutCount=1;pl.pSetLayouts=&descriptor_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"vkCreatePipelineLayout(mesh)");
        VkShaderModule vs{},fs{};
        try{
            vs=shader(mesh_vert_spv,sizeof(mesh_vert_spv));fs=shader(mesh_frag_spv,sizeof(mesh_frag_spv));
            std::array<VkPipelineShaderStageCreateInfo,2> stages{};for(auto& s:stages)s=info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
            stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
            // Both stages share the fixed pipeline mode. The mip0 variant can
            // discard mesh/derivative work; projection discards conversion.
            std::uint32_t render_mode=0;
            const VkSpecializationMapEntry mode_entry{0,0,sizeof(render_mode)};
            const VkSpecializationInfo specialization{1,&mode_entry,sizeof(render_mode),&render_mode};
            for(auto& stage:stages)stage.pSpecializationInfo=&specialization;
            VkVertexInputBindingDescription vb{0,sizeof(ProjectionVertex),VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array<VkVertexInputAttributeDescription,3> attrs{{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(ProjectionVertex,position)},
                {1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ProjectionVertex,uv)},{2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(ProjectionVertex,normal)}}};
            auto vi=info<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&vb;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=attrs.data();
            auto ia=info<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            auto vp=info<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);vp.viewportCount=vp.scissorCount=1;
            auto rs=info<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
            auto ms=info<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);ms.rasterizationSamples=samples;
            auto ds=info<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState ba{};ba.blendEnable=VK_TRUE;ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp=VK_BLEND_OP_ADD;ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.alphaBlendOp=VK_BLEND_OP_ADD;ba.colorWriteMask=15;
            auto bs=info<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);bs.attachmentCount=1;bs.pAttachments=&ba;
            const VkDynamicState dyns[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};auto dyn=info<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);dyn.dynamicStateCount=2;dyn.pDynamicStates=dyns;
            auto gp=info<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);gp.stageCount=2;gp.pStages=stages.data();gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;
            gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pDepthStencilState=&ds;gp.pColorBlendState=&bs;gp.pDynamicState=&dyn;gp.layout=layout;gp.renderPass=pass;
            check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline),"vkCreateGraphicsPipelines(mesh)");
            if(quality_supported){
                // Quality sources already contain premultiplied linear color.
                render_mode=1;
                ba.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&quality_pipeline),"vkCreateGraphicsPipelines(quality mesh)");
                ba.blendEnable=VK_FALSE;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
                ds.depthTestEnable=ds.depthWriteEnable=VK_FALSE;
                render_mode=2;
                gp.renderPass=mip_pass;
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&mip_pipeline),"vkCreateGraphicsPipelines(mip0)");
                render_mode=4;
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&nv12_linear_mip_pipeline),"vkCreateGraphicsPipelines(linear NV12 mip0)");
                render_mode=6;
                check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&nv12_linear_base_pipeline),"vkCreateGraphicsPipelines(linear NV12 baseline)");
                if(image_format_list){render_mode=3;gp.renderPass=nv12_mip_pass;
                    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&nv12_mip_pipeline),"vkCreateGraphicsPipelines(NV12 mip0)");
                    render_mode=5;
                    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&nv12_base_pipeline),"vkCreateGraphicsPipelines(NV12 baseline)");}
            }
        }catch(...){if(vs)vkDestroyShaderModule(device,vs,nullptr);
        if(fs)vkDestroyShaderModule(device,fs,nullptr);
            throw;
        }
        vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
        auto si=info<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
        check(vkCreateSampler(device,&si,nullptr,&sampler),"vkCreateSampler(mesh)");
        if(quality_supported){si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;si.maxLod=VK_LOD_CLAMP_NONE;
            // Full center derivatives select the mip footprint. No extra bias
            // or anisotropy; the view limits sampling to the owned mip chain.
            check(vkCreateSampler(device,&si,nullptr,&mip_sampler),"vkCreateSampler(quality mesh)");}
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,max_sources*4};auto dpi=info<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        dpi.maxSets=max_sources*2;dpi.poolSizeCount=1;dpi.pPoolSizes=&ps;check(vkCreateDescriptorPool(device,&dpi,nullptr,&descriptors),"vkCreateDescriptorPool(mesh)");
        auto pc=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&pc,nullptr,&pool),"vkCreateCommandPool(mesh)");
        auto ca=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&command),"vkAllocateCommandBuffers(mesh)");auto fi=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(device,&fi,nullptr,&fence),"vkCreateFence(mesh)");
        auto bi=info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);bi.size=sizeof(ProjectionVertex)*max_vertices;bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device,&bi,nullptr,&vertices),"vkCreateBuffer(mesh)");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,vertices,&req);
        auto ma=info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);ma.allocationSize=req.size;ma.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);coherent=(mp.memoryTypes[ma.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        check(vkAllocateMemory(device,&ma,nullptr,&vertex_memory),"vkAllocateMemory(mesh vertices)");check(vkBindBufferMemory(device,vertices,vertex_memory,0),"vkBindBufferMemory(mesh)");
        check(vkMapMemory(device,vertex_memory,0,VK_WHOLE_SIZE,0,&mapped),"vkMapMemory(mesh)");
    }

VkFramebuffer ProjectionRenderer::Impl::framebuffer(VkImageView view){
        auto found=framebuffers.find(view);
        if(found!=framebuffers.end())return found->second;
        std::array<VkImageView,3> at{{samples==VK_SAMPLE_COUNT_1_BIT?view:color.view,depth.view,view}};
        auto ci=info<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);ci.renderPass=pass;ci.attachmentCount=samples==VK_SAMPLE_COUNT_1_BIT?2:3;
        ci.pAttachments=at.data();ci.width=width;ci.height=height;ci.layers=1;VkFramebuffer fb{};
        check(vkCreateFramebuffer(device,&ci,nullptr,&fb),"vkCreateFramebuffer(mesh)");framebuffers.emplace(view,fb);return fb;
    }

void ProjectionRenderer::Impl::render(const std::vector<ProjectionSource>& sources,const std::array<ProjectionEye,2>& eyes,ProjectionSampling sampling,std::uint64_t display_frame){
        (void)display_frame;
        stats.last_mip_gpu_ms=stats.last_projection_gpu_ms=-1;
        if(sources.empty()||sources.size()>max_sources)throw std::runtime_error("Invalid projection source count");
        if(sampling!=ProjectionSampling::Baseline&&sampling!=ProjectionSampling::QualitySS)throw std::runtime_error("Invalid projection sampling mode");
        const bool quality=sampling==ProjectionSampling::QualitySS;
        const bool timed=timestamps&&(stats.baseline_frames+stats.quality_frames)%timestamp_interval==0;
        if(quality&&!quality_supported)throw std::runtime_error("Quality SS requires filterable sRGB mip attachments");
        std::array<bool,max_sources> update{},cached{};
        std::array<unsigned,max_sources> offsets{},counts{};std::vector<ProjectionVertex> mesh;
        for(unsigned i=0;i<sources.size();++i){if(!sources[i].image||!sources[i].view)throw std::runtime_error("Null projection source");
            for(unsigned j=0;j<i;++j)if(sources[j].image==sources[i].image)throw std::runtime_error("Duplicate projection source image");
            const auto& input=sources[i];
            if(input.chroma_view||input.chroma_image){
                const auto r=input.source_rect,c=input.yuv_coefficients;
                if(!quality_supported||!input.chroma_view||!input.chroma_image||!std::all_of(r.begin(),r.end(),[](float v){return std::isfinite(v);})||
                   r[0]<0||r[1]<0||r[2]<=0||r[3]<=0||r[0]+r[2]>1.00001f||r[1]+r[3]>1.00001f||
                   !std::all_of(c.begin(),c.end(),[](float v){return std::isfinite(v);})||c[0]<=0||c[1]<=0||c[0]+c[1]>=1||c[2]<0||c[2]>.1f||c[3]<1||c[3]>1.2f)
                    throw std::runtime_error("Invalid NV12 projection source");
            }
            cached[i]=quality||input.chroma_view;
            if(cached[i]){const auto& source=sources[i];
                if(!source.width||!source.height||source.width>8192||source.height>8192||std::uint64_t{source.width}*source.height>16'777'216||
                   source.cache_slot>=max_sources||!source.content_generation)throw std::runtime_error("Invalid quality projection source");
                for(unsigned j=0;j<i;++j)if(sources[j].cache_slot==source.cache_slot)throw std::runtime_error("Duplicate projection cache slot");
                auto& cache=caches[source.cache_slot];prepare_cache(cache,source,quality);
                update[i]=!cache.valid||cache.generation!=source.content_generation;
            }
            offsets[i]=static_cast<unsigned>(mesh.size());auto m=projection_panel_mesh(sources[i].panel,sources[i].panel.curvature_radians>.001f?segments:1);counts[i]=static_cast<unsigned>(m.size());mesh.insert(mesh.end(),m.begin(),m.end());}
        std::memcpy(mapped,mesh.data(),mesh.size()*sizeof(ProjectionVertex));
        if(!coherent){auto range=info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);range.memory=vertex_memory;range.size=VK_WHOLE_SIZE;check(vkFlushMappedMemoryRanges(device,1,&range),"vkFlushMappedMemoryRanges(mesh)");}
        check(vkResetDescriptorPool(device,descriptors,0),"vkResetDescriptorPool(mesh)");
        std::array<VkDescriptorSet,max_sources*2> sets{};std::array<VkDescriptorSetLayout,max_sources*2> layouts{};layouts.fill(descriptor_layout);
        const auto source_count=static_cast<unsigned>(sources.size());
        auto ai=info<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);ai.descriptorPool=descriptors;ai.descriptorSetCount=source_count*2;ai.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device,&ai,sets.data()),"vkAllocateDescriptorSets(mesh)");
        for(unsigned i=0;i<ai.descriptorSetCount;++i){const bool owned=cached[i%source_count]&&i<source_count;const auto& source=sources[i%source_count];
            const bool nv12=!owned&&source.chroma_view;
            VkDescriptorImageInfo im{owned&&quality?mip_sampler:sampler,owned?caches[source.cache_slot].image.view:source.view,
                nv12?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo chroma=im;if(nv12)chroma.imageView=source.chroma_view;
            std::array<VkWriteDescriptorSet,2> writes{};
            for(unsigned b=0;b<2;++b){auto& w=writes[b];w=info<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                w.dstSet=sets[i];w.dstBinding=b;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=b?&chroma:&im;}
            vkUpdateDescriptorSets(device,static_cast<unsigned>(writes.size()),writes.data(),0,nullptr);}
        std::array<VkFramebuffer,2> fb{};std::array<Camera,2> cameras{};
        for(unsigned e=0;e<2;++e){if(!eyes[e].target)throw std::runtime_error("Null projection target");fb[e]=framebuffer(eyes[e].target);
            cameras[e]={projection_view_matrix(eyes[e].pose,eyes[e].fov),{eyes[e].pose.position.x,eyes[e].pose.position.y,eyes[e].pose.position.z,1},
                        {0,0,0,0}};}
        check(vkResetCommandBuffer(command,0),"vkResetCommandBuffer(mesh)");check(vkResetFences(device,1,&fence),"vkResetFences(mesh)");
        auto begin=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&begin),"vkBeginCommandBuffer(mesh)");
        if(timed)vkCmdResetQueryPool(command,timestamps,0,3);
        source_transitions(sources,update,quality,true);
        const bool updated_mips=std::any_of(update.begin(),update.end(),[](bool value){return value;});
        const auto boundary=[&](unsigned query){if(timed){
            // BOTTOM records completion of the preceding source dependency or
            // mip pass. The execution boundary prevents following setup or
            // fragments from overlapping the timestamp, making spans disjoint.
            // There is no additional CPU wait.
            vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,query);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0,0,nullptr,0,nullptr,0,nullptr);
        }};
        boundary(updated_mips?0:1);
        for(unsigned i=0;i<sources.size();++i)if(update[i])update_mips(caches[sources[i].cache_slot],sets[source_count+i],sources[i]);
        if(updated_mips)boundary(1);
        for(unsigned e=0;e<2;++e){
            std::array<VkClearValue,2> clear{};clear[1].depthStencil={1,0};auto rp=info<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);rp.renderPass=pass;rp.framebuffer=fb[e];rp.renderArea.extent={width,height};rp.clearValueCount=2;rp.pClearValues=clear.data();
            vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,quality?quality_pipeline:pipeline);
            const VkViewport viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};const VkRect2D scissor{{0,0},{width,height}};
            vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);const VkDeviceSize offset=0;vkCmdBindVertexBuffers(command,0,1,&vertices,&offset);
            vkCmdPushConstants(command,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Camera),&cameras[e]);
            // Back-to-front alpha edges; depth retains correct opaque overlap.
            std::vector<unsigned> order;for(unsigned i=0;i<sources.size();++i)order.push_back(i);
            const auto distance=[&](unsigned i){const auto a=sources[i].panel.pose.position,b=eyes[e].pose.position;return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z);};
            std::stable_sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return distance(a)>distance(b);});
            for(unsigned i:order){vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[i],0,nullptr);vkCmdDraw(command,counts[i],1,offsets[i],0);}
            vkCmdEndRenderPass(command);
        }
        source_transitions(sources,update,quality,false);
        if(timed)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,2);
        check(vkEndCommandBuffer(command),"vkEndCommandBuffer(mesh)");auto submit=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        check(vkQueueSubmit(queue,1,&submit,fence),"vkQueueSubmit(mesh)");const auto start=std::chrono::steady_clock::now();
        const auto result=vkWaitForFences(device,1,&fence,VK_TRUE,2'000'000'000ULL);wait_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(result!=VK_SUCCESS){vkDeviceWaitIdle(device);check(result,"vkWaitForFences(mesh,2s)");}
        if(timed){
            std::array<std::uint64_t,3> values{};const unsigned first=updated_mips?0:1;
            if(vkGetQueryPoolResults(device,timestamps,first,3-first,(3-first)*sizeof(values[0]),values.data()+first,sizeof(values[0]),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
                const auto mask=timestamp_bits>=64?UINT64_MAX:(std::uint64_t{1}<<timestamp_bits)-1;
                const auto duration=[&](unsigned a,unsigned b){return double((values[b]-values[a])&mask)*timestamp_tick_ns/1'000'000.;};
                stats.last_mip_gpu_ms=updated_mips?duration(0,1):0;
                stats.last_projection_gpu_ms=duration(1,2);
                ++stats.projection_gpu_samples;stats.projection_gpu_ms_sum+=stats.last_projection_gpu_ms;
                stats.projection_gpu_ms_max=std::max(stats.projection_gpu_ms_max,stats.last_projection_gpu_ms);
                if(updated_mips){++stats.mip_gpu_samples;stats.mip_gpu_ms_sum+=stats.last_mip_gpu_ms;
                    stats.mip_gpu_ms_max=std::max(stats.mip_gpu_ms_max,stats.last_mip_gpu_ms);}
            }
        }
        if(quality)++stats.quality_frames;else ++stats.baseline_frames;
        for(unsigned i=0;i<sources.size();++i)if(cached[i]){auto& cache=caches[sources[i].cache_slot];
            if(update[i]){cache.valid=cache.initialized=true;cache.generation=sources[i].content_generation;
                if(quality){++stats.mip_updates;
                    if(sources[i].chroma_view){++stats.nv12_mip_updates;stats.nv12_encoded_mip_updates+=cache.raw;}}
                else{++stats.nv12_base_updates;stats.nv12_encoded_base_updates+=cache.raw;}
                if(sources[i].chroma_view)stats.nv12_intermediate_bytes_avoided+=std::uint64_t{cache.width}*cache.height*4;
            }else if(quality)++stats.mip_reuses;else ++stats.nv12_base_reuses;
        }
    }

ProjectionRenderer::Impl::~Impl(){
        if(!device)return;
        vkDeviceWaitIdle(device);
        for(const auto& item:framebuffers)vkDestroyFramebuffer(device,item.second,nullptr);
        for(auto& cache:caches)release_cache(cache);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(quality_pipeline)vkDestroyPipeline(device,quality_pipeline,nullptr);
        if(mip_pipeline)vkDestroyPipeline(device,mip_pipeline,nullptr);
        if(nv12_mip_pipeline)vkDestroyPipeline(device,nv12_mip_pipeline,nullptr);
        if(nv12_linear_mip_pipeline)vkDestroyPipeline(device,nv12_linear_mip_pipeline,nullptr);
        if(nv12_base_pipeline)vkDestroyPipeline(device,nv12_base_pipeline,nullptr);
        if(nv12_linear_base_pipeline)vkDestroyPipeline(device,nv12_linear_base_pipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(descriptor_layout)vkDestroyDescriptorSetLayout(device,descriptor_layout,nullptr);
        if(sampler)vkDestroySampler(device,sampler,nullptr);
        if(mip_sampler)vkDestroySampler(device,mip_sampler,nullptr);
        if(pass)vkDestroyRenderPass(device,pass,nullptr);
        if(mip_pass)vkDestroyRenderPass(device,mip_pass,nullptr);
        if(nv12_mip_pass)vkDestroyRenderPass(device,nv12_mip_pass,nullptr);
        if(timestamps)vkDestroyQueryPool(device,timestamps,nullptr);
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
        if(mapped)vkUnmapMemory(device,vertex_memory);
        if(vertices)vkDestroyBuffer(device,vertices,nullptr);
        if(vertex_memory)vkFreeMemory(device,vertex_memory,nullptr);
        for(auto* im:{&color,&depth}){if(im->view)vkDestroyImageView(device,im->view,nullptr);
        if(im->image)vkDestroyImage(device,im->image,nullptr);
        if(im->memory)vkFreeMemory(device,im->memory,nullptr);}
    }
ProjectionRenderer::ProjectionRenderer(VkPhysicalDevice p,VkDevice d,VkQueue q,unsigned f,VkFormat fmt,unsigned w,unsigned h,unsigned max_samples,unsigned timestamp_period,bool image_format_list):impl_(std::make_unique<Impl>()){
    impl_->physical=p;impl_->device=d;impl_->queue=q;impl_->family=f;impl_->format=fmt;impl_->width=w;impl_->height=h;impl_->max_samples=max_samples;
    impl_->image_format_list=image_format_list;impl_->timestamp_interval=timestamp_period;impl_->initialize();}
ProjectionRenderer::~ProjectionRenderer()=default;
void ProjectionRenderer::render(const std::vector<ProjectionSource>& s,const std::array<ProjectionEye,2>& e,ProjectionSampling sampling,std::uint64_t display_frame){impl_->render(s,e,sampling,display_frame);}
bool ProjectionRenderer::can_reuse_source(unsigned slot,unsigned width,unsigned height,std::uint64_t generation,ProjectionSampling sampling)const{
    if(slot>=max_sources)throw std::runtime_error("Invalid projection cache slot");
    if(sampling!=ProjectionSampling::Baseline&&sampling!=ProjectionSampling::QualitySS)throw std::runtime_error("Invalid projection sampling mode");
    const auto& cache=impl_->caches[slot];
    return width&&height&&generation&&cache.valid&&cache.initialized&&cache.image.image&&
        cache.width==width&&cache.height==height&&cache.generation==generation&&cache.quality==(sampling==ProjectionSampling::QualitySS);
}
void ProjectionRenderer::invalidate(unsigned slot){if(slot>=max_sources)throw std::runtime_error("Invalid projection cache slot");impl_->caches[slot].valid=false;}
ProjectionSamplingStats ProjectionRenderer::statistics()const noexcept{
    auto result=impl_->stats;
    for(const auto& cache:impl_->caches)if(cache.image.image){result.cached_bytes+=cache.quality?mip_bytes(cache.width,cache.height):std::uint64_t{cache.width}*cache.height*4;result.cached_levels+=cache.levels;}
    return result;
}
unsigned ProjectionRenderer::samples()const noexcept{return static_cast<unsigned>(impl_->samples);}
double ProjectionRenderer::last_gpu_wait_ms()const noexcept{return impl_->wait_ms;}
}
