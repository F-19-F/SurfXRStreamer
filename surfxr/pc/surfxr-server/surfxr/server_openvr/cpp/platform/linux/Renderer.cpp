#include "Renderer.hpp"

#include <optional>
#include <unistd.h>
#include <vulkan/vulkan_structs.hpp>

namespace surfxr::render::detail {

RenderPipeline::RenderPipeline(
    VkContext const& ctx, vk::DescriptorSetLayout& layout, PipelineCreateInfo& pipelineCI
) {
    vk::ShaderModuleCreateInfo shaderCI {
        .codeSize = pipelineCI.shaderData.size(),
        .pCode = reinterpret_cast<u32 const*>(pipelineCI.shaderData.data()),
    };
    shader = ctx.dev.createShaderModule(shaderCI);

    // Every pass shares one push-constant range; shaders that do not declare
    // the block simply ignore it.
    vk::PushConstantRange pushRange {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    vk::PipelineLayoutCreateInfo pipeLayoutCI {
        .setLayoutCount = 1,
        .pSetLayouts = &layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    pipeLayout = ctx.dev.createPipelineLayout(pipeLayoutCI);

    vk::SpecializationInfo specInfo {
        .mapEntryCount = static_cast<u32>(pipelineCI.specs.size()),
        .pMapEntries = pipelineCI.specs.data(),
        .dataSize = pipelineCI.specData.size(),
        .pData = pipelineCI.specData.data(),
    };

    vk::PipelineShaderStageCreateInfo stageCI {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = shader,
        .pName = "main",
        .pSpecializationInfo = not pipelineCI.specs.empty() ? &specInfo : nullptr,
    };
    vk::ComputePipelineCreateInfo pipeCI {
        .stage = stageCI,
        .layout = pipeLayout,
    };
    auto [result, pipes] = ctx.dev.createComputePipelines({}, pipeCI);

    assert(result == vk::Result::eSuccess);
    pipe = pipes[0];
};

void RenderPipeline::render(
    VkContext const& ctx,
    vk::CommandBuffer cmdBuf,
    vk::ImageView in,
    vk::ImageView out,
    vk::Extent2D outSize,
    PushConstants const& pushConstants
) {
    cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipe);
    cmdBuf.pushConstants(
        pipeLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushConstants), &pushConstants
    );

    vk::DescriptorImageInfo descImgInfoIn {
        .imageView = in,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::DescriptorImageInfo descImgInfoOut {
        .imageView = out,
        .imageLayout = vk::ImageLayout::eGeneral,
    };

    vk::WriteDescriptorSet descWriteSets[2];
    descWriteSets[0] = {
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &descImgInfoIn,
    };
    descWriteSets[1] = {
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .pImageInfo = &descImgInfoOut,
    };
    cmdBuf.pushDescriptorSetKHR(
        vk::PipelineBindPoint::eCompute, pipeLayout, 0, descWriteSets, ctx.dispatch
    );

    cmdBuf.dispatch((outSize.width + 7) / 8, (outSize.height + 7) / 8, 1);
}

void RenderPipeline::destroy(VkContext const& ctx) {
    ctx.dev.destroy(pipe);
    ctx.dev.destroy(pipeLayout);
    ctx.dev.destroy(shader);
}

}

#include "RendererImages.inl"

namespace surfxr::render {

Renderer::Renderer(
    VkContext const& vkCtx, RendererCreateInfo& createInfo, std::vector<PipelineCreateInfo> pipeCIs
) {
    eyeExtent = createInfo.inputEyeExtent;
    outExtent = createInfo.outputExtent;

    // The import must match how SteamVR created the image: its real format
    // and MUTABLE_FORMAT (passed in CreateSwapTextureSet), and no format list
    // since SteamVR's image has none. A list could let the driver choose a
    // different memory layout (e.g. compression) than the exporter did.
    // MUTABLE_FORMAT is what makes the UNORM view of an sRGB image legal.
    vk::Format inputFormat = createInfo.format;
    vk::Format unormFormat = createInfo.format;
    if (inputFormat == vk::Format::eR8G8B8A8Srgb)
        unormFormat = vk::Format::eR8G8B8A8Unorm;

    if (inputFormat == vk::Format::eB8G8R8A8Srgb)
        unormFormat = vk::Format::eB8G8R8A8Unorm;

    // TODO: Only put the usage flags for the images that need to have them
    vk::ImageCreateInfo inputImgCI {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = inputFormat,

        .extent = {
            .width = eyeExtent.width,
            .height = eyeExtent.height,
            .depth = 1,
        },

        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eTransferSrc
            | vk::ImageUsageFlagBits::eInputAttachment | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    for (int i = 0; i < ImageCount; ++i) {
        inputImages[i] = createImage(vkCtx, inputImgCI, createInfo.inputImgFds[i], unormFormat);
    }

    // Everything downstream is UNORM regardless of the input format: the
    // copy moves raw bytes, the shaders imageStore through rgba8, and the
    // encoder consumes raw bytes with no notion of sRGB.
    auto stagingImgCI = inputImgCI;
    stagingImgCI.flags = {};
    stagingImgCI.format = unormFormat;
    stagingImgCI.extent.width = eyeExtent.width * 2;
    stagingImgCI.usage = inputImgCI.usage | vk::ImageUsageFlagBits::eTransferDst
        | vk::ImageUsageFlagBits::eStorage;

    for (auto& img : stagingImgs) {
        img = createImage(vkCtx, stagingImgCI);
    }

    // VAAPI imports through DRM-PRIME and wants a dma-buf; NvEnc imports
    // through CUDA interop and wants an opaque fd.
    output = createOutputImage(
        vkCtx,
        createInfo.outputExtent,
        stagingImgCI.format,
        vkCtx.meta.vendor == Vendor::Nvidia ? HandleType::OpaqueFd : HandleType::DmaBuf
    );

    vk::QueryPoolCreateInfo poolCI {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = 2,
    };
    timestampPool = vkCtx.dev.createQueryPool(poolCI);

    vk::CommandPoolCreateInfo cmdPoolCI {
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = vkCtx.meta.queueFamily,
    };
    cmdPool = vkCtx.dev.createCommandPool(cmdPoolCI);

    vk::CommandBufferAllocateInfo cmdBufAllocInfo {
        .commandPool = cmdPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    cmdBuf = vkCtx.dev.allocateCommandBuffers(cmdBufAllocInfo)[0];

    vk::SamplerCreateInfo samplerCI {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,

        .anisotropyEnable = true,
        .maxAnisotropy = 16.f,

        .borderColor = vk::BorderColor::eFloatTransparentBlack,
    };
    sampler = vkCtx.dev.createSampler(samplerCI);

    vk::DescriptorSetLayoutBinding descBindings[2];
    descBindings[0] = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .pImmutableSamplers = &sampler,
    };
    descBindings[1] = {
        .binding = 1,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };

    vk::DescriptorSetLayoutCreateInfo descLayoutCI {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 2,
        .pBindings = descBindings,
    };
    descLayout = vkCtx.dev.createDescriptorSetLayout(descLayoutCI);

    pipes.reserve(pipeCIs.size());
    for (auto& pipeCI : pipeCIs) {
        pipes.emplace_back(vkCtx, descLayout, pipeCI);
    }

    vk::SemaphoreTypeCreateInfo timelineCI {
        .semaphoreType = vk::SemaphoreType::eTimeline,
        .initialValue = 0,
    };
    vk::SemaphoreCreateInfo semCI {
        .pNext = &timelineCI,
    };
    // renderFinishedSem = vkCtx.dev.createSemaphore(semCI);

    fence = vkCtx.dev.createFence({});
}

void Renderer::render(VkContext& vkCtx, u32 leftIdx, u32 rightIdx, int const waitFds[2]) {
    // Import the input writers' sync files as temporary binary semaphores;
    // the submission below waits on them at the transfer stage, so the eye
    // copies start exactly when the writes complete.
    vk::Semaphore importedSems[2] = {};
    u32 importedCount = 0;
    if (waitFds != nullptr) {
        for (int e = 0; e < 2; ++e) {
            if (waitFds[e] < 0) {
                continue;
            }
            vk::Semaphore sem = vkCtx.dev.createSemaphore({});
            vk::ImportSemaphoreFdInfoKHR importInfo {
                .semaphore = sem,
                .flags = vk::SemaphoreImportFlagBits::eTemporary,
                .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
                .fd = waitFds[e],
            };
            if (vkCtx.dev.importSemaphoreFdKHR(&importInfo, vkCtx.dispatch)
                == vk::Result::eSuccess) {
                importedSems[importedCount++] = sem;
            } else {
                vkCtx.dev.destroy(sem);
                close(waitFds[e]);
            }
        }
    }

    vk::CommandBufferBeginInfo beginInfo {};
    cmdBuf.begin(beginInfo);

    cmdBuf.resetQueryPool(timestampPool, 0, 2);
    cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, timestampPool, 0);

    auto flushBarriers = [&](std::vector<vk::ImageMemoryBarrier>& barriers,
                             vk::PipelineStageFlags srcStage,
                             vk::PipelineStageFlags dstStage) {
        if (!barriers.empty()) {
            cmdBuf.pipelineBarrier(srcStage, dstStage, {}, {}, {}, barriers);
            barriers.clear();
        }
    };

    auto transitionImage = [&](Image& image,
                               vk::ImageLayout newLayout,
                               vk::AccessFlags srcAccessMask,
                               vk::AccessFlags dstAccessMask,
                               std::vector<vk::ImageMemoryBarrier>& barriers) {
        if (image.layout == newLayout) {
            return;
        }

        barriers.push_back(vk::ImageMemoryBarrier {
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = image.layout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image,
            .subresourceRange {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        });
        image.layout = newLayout;
    };

    std::vector<vk::ImageMemoryBarrier> barriers;
    Image* prev = nullptr;

    // Copy the left and right eye images into the first working image.
    Image* stageOut = pipes.empty() ? &output.image : &stagingImgs[0];
    transitionImage(
        inputImages[leftIdx],
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eNone,
        vk::AccessFlagBits::eTransferRead,
        barriers
    );
    transitionImage(
        inputImages[rightIdx],
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eNone,
        vk::AccessFlagBits::eTransferRead,
        barriers
    );
    transitionImage(
        *stageOut,
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eNone,
        vk::AccessFlagBits::eTransferWrite,
        barriers
    );

    flushBarriers(
        barriers, vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer
    );

    vk::ImageSubresourceLayers subresLayout {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    vk::ImageCopy2KHR leftCopy {
        .srcSubresource = subresLayout,
        .srcOffset = {},
        .dstSubresource = subresLayout,
        .dstOffset = {},
        .extent = {
            .width = eyeExtent.width,
            .height = eyeExtent.height,
            .depth = 1,
        },
    };
    vk::CopyImageInfo2KHR leftCopyInfo {
        .srcImage = inputImages[leftIdx].image,
        .srcImageLayout = inputImages[leftIdx].layout,
        .dstImage = stageOut->image,
        .dstImageLayout = stageOut->layout,
        .regionCount = 1,
        .pRegions = &leftCopy,
    };
    cmdBuf.copyImage2KHR(leftCopyInfo, vkCtx.dispatch);

    auto rightCopy = leftCopy;
    rightCopy.dstOffset = vk::Offset3D {
        .x = static_cast<int32_t>(eyeExtent.width),
        .y = 0,
        .z = 0,
    };
    vk::CopyImageInfo2KHR rightCopyInfo {
        .srcImage = inputImages[rightIdx].image,
        .srcImageLayout = inputImages[rightIdx].layout,
        .dstImage = stageOut->image,
        .dstImageLayout = stageOut->layout,
        .regionCount = 1,
        .pRegions = &rightCopy,
    };
    cmdBuf.copyImage2KHR(rightCopyInfo, vkCtx.dispatch);

    prev = stageOut;

    PushConstants const pushConstants {
        .warp = warpParams,
        .centerShifts = foveationCenterShifts,
    };

    for (usize pipeIdx = 0; pipeIdx < pipes.size(); ++pipeIdx) {
        Image* nextOut = pipeIdx + 1 == pipes.size()
            ? &output.image
            : &stagingImgs[(pipeIdx + 1) % StagingImgCount];
        vk::Extent2D targetExtent = pipeIdx + 1 == pipes.size() ? outExtent
                                                                : vk::Extent2D {
                                                                      .width = eyeExtent.width * 2,
                                                                      .height = eyeExtent.height,
                                                                  };

        transitionImage(
            *prev,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead,
            barriers
        );
        transitionImage(
            *nextOut,
            vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eNone,
            vk::AccessFlagBits::eShaderWrite,
            barriers
        );

        flushBarriers(
            barriers,
            vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader
        );

        pipes[pipeIdx].render(
            vkCtx, cmdBuf, prev->view, nextOut->view, targetExtent, pushConstants
        );
        prev = nextOut;
    }

    cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timestampPool, 1);

    cmdBuf.end();

    // vk::TimelineSemaphoreSubmitInfo timelineInfo {
    //     .waitSemaphoreValueCount = 1,
    //     .pWaitSemaphoreValues = &waitValue,
    // };
    vk::PipelineStageFlags waitStages[2] = {
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer,
    };
    vk::SubmitInfo submitInfo {
        // .pNext = &timelineInfo,
        .waitSemaphoreCount = importedCount, .pWaitSemaphores = importedSems,
        .pWaitDstStageMask = waitStages,     .commandBufferCount = 1,
        .pCommandBuffers = &cmdBuf,
    };

    vkCtx.useQueue([&](auto& queue) { queue.submit(submitInfo, fence); });
    // The fence wait below also orders the destruction of the temporary
    // imported semaphores: the queue waits on them until the submission
    // finishes, so destroying them earlier would be a use after free.
    assert(vkCtx.dev.waitForFences(fence, true, UINT64_MAX) == vk::Result::eSuccess);
    vkCtx.dev.resetFences(fence);

    for (u32 i = 0; i < importedCount; ++i) {
        vkCtx.dev.destroy(importedSems[i]);
    }
}

void Renderer::destroy(VkContext const& ctx) {
    // ctx.dev.destroy(renderFinishedSem);

    for (auto& pipe : pipes) {
        pipe.destroy(ctx);
    }

    ctx.dev.destroy(fence);
    ctx.dev.destroy(descLayout);
    ctx.dev.destroy(sampler);

    ctx.dev.destroy(cmdPool);
    ctx.dev.destroy(timestampPool);

    output.image.destroy(ctx);

    for (auto& img : stagingImgs) {
        img.destroy(ctx);
    }

    for (auto& img : inputImages) {
        img.destroy(ctx);
    }
}

}
