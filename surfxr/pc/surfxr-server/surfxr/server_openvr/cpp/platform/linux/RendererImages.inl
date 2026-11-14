#ifndef DRM_FORMAT_INVALID
#define DRM_FORMAT_INVALID 0
#define fourcc_code(a, b, c, d)                                                                    \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ARGB8888 fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888 fourcc_code('A', 'B', '2', '4')
#define fourcc_mod_code(vendor, val) ((((uint64_t)vendor) << 56) | ((val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_INVALID fourcc_mod_code(0, ((1ULL << 56) - 1))
#define DRM_FORMAT_MOD_LINEAR fourcc_mod_code(0, 0)
#define DRM_FORMAT_MOD_VENDOR_AMD 0x02
#define AMD_FMT_MOD_DCC_SHIFT 13
#define AMD_FMT_MOD_DCC_MASK 0x1
#define IS_AMD_FMT_MOD(val) (((val) >> 56) == DRM_FORMAT_MOD_VENDOR_AMD)
#define AMD_FMT_MOD_GET(field, value)                                                              \
    (((value) >> AMD_FMT_MOD_##field##_SHIFT) & AMD_FMT_MOD_##field##_MASK)
#endif

static bool filter_modifier(uint64_t modifier) {
    if (IS_AMD_FMT_MOD(modifier)) {
        // DCC not supported as encode input
        if (AMD_FMT_MOD_GET(DCC, modifier)) {
            return false;
        }
    }
    return true;
}

static u32 to_drm_format(vk::Format format) {
    switch (format) {
    case vk::Format::eB8G8R8A8Unorm:
        return DRM_FORMAT_ARGB8888;
    case vk::Format::eR8G8B8A8Unorm:
        return DRM_FORMAT_ABGR8888;
    default:
        // assert(false);
        return DRM_FORMAT_INVALID;
    }
}

using namespace surfxr;
using namespace surfxr::render;

u32 memoryTypeIndex(VkContext const& ctx, vk::MemoryPropertyFlags properties, u32 typeBits) {
    auto prop = ctx.physDev.getMemoryProperties();
    for (u32 i = 0; i < prop.memoryTypeCount; ++i) {
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && typeBits & (1 << i)) {
            return i;
        }
    }
    throw std::runtime_error("No matching memoryTypeIndex found");
}

// Create or import Image, depending on whether an fd is passed. viewFormat
// overrides the view's format; the image must be MUTABLE_FORMAT for that to
// be legal when it differs from imageCI.format.
inline Image createImage(
    VkContext const& ctx,
    vk::ImageCreateInfo imageCI,
    std::optional<int> fd = std::nullopt,
    std::optional<vk::Format> viewFormat = std::nullopt
) {
    vk::ExternalMemoryImageCreateInfo extMemImgCI {
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd,
    };
    if (fd.has_value()) {
        extMemImgCI.pNext = imageCI.pNext;
        imageCI.pNext = &extMemImgCI;
    }

    Image img {
        .image = ctx.dev.createImage(imageCI),
        .layout = imageCI.initialLayout,
    };

    vk::MemoryDedicatedRequirements dedicatedReqs {};
    vk::MemoryRequirements2 memReqs {
        .pNext = &dedicatedReqs,
    };
    vk::ImageMemoryRequirementsInfo2 memReqInfo {
        .image = img.image,
    };
    ctx.dev.getImageMemoryRequirements2(&memReqInfo, &memReqs);

    vk::ImportMemoryFdInfoKHR importMemInfo {
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd,
        .fd = fd.value_or(-1),
    };
    vk::MemoryDedicatedAllocateInfo memDedicatedInfo {
        .pNext = fd.has_value() ? &importMemInfo : nullptr,
        .image = img.image,
    };
    vk::MemoryAllocateInfo memAllocInfo {
        .pNext = &memDedicatedInfo,
        .allocationSize = memReqs.memoryRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(
            ctx, vk::MemoryPropertyFlagBits::eDeviceLocal, memReqs.memoryRequirements.memoryTypeBits
        ),
    };
    img.memory = ctx.dev.allocateMemory(memAllocInfo);

    vk::BindImageMemoryInfo bindImgInfo {
        .image = img.image,
        .memory = img.memory,
        .memoryOffset = 0,
    };
    ctx.dev.bindImageMemory2(bindImgInfo);

    vk::ImageViewCreateInfo imgViewCI {
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = viewFormat.value_or(imageCI.format),
        .components {
            .r = vk::ComponentSwizzle::eIdentity,
            .g = vk::ComponentSwizzle::eIdentity,
            .b = vk::ComponentSwizzle::eIdentity,
            .a = vk::ComponentSwizzle::eIdentity,
        },
        .subresourceRange {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = imageCI.mipLevels,
            .baseArrayLayer = 0,
            .layerCount = imageCI.arrayLayers,
        },
    };
    img.view = ctx.dev.createImageView(imgViewCI);

    return img;
}

Output createOutputImage(
    VkContext const& ctx, vk::Extent2D extent, vk::Format format, HandleType handleType
) {
    Image img;
    Output out;

    vk::ImageCreateInfo imgCI {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    img.layout = imgCI.initialLayout;

    vk::ExternalMemoryImageCreateInfo extImgCI {};

    // Lifetime
    std::vector<uint64_t> imageModifiers;

    // TODO: Actually import this
    bool haveDmaBuf = true;
    bool haveDrmModifiers = true;
    std::vector<vk::DrmFormatModifierPropertiesEXT> modProps;

    if (haveDrmModifiers && handleType == HandleType::DmaBuf) {
        imgCI.tiling = vk::ImageTiling::eDrmFormatModifierEXT;

        vk::DrmFormatModifierPropertiesListEXT modPropList {};
        vk::FormatProperties2 formatProps {
            .pNext = &modPropList,
        };
        ctx.physDev.getFormatProperties2(imgCI.format, &formatProps);

        modProps.resize(modPropList.drmFormatModifierCount);
        modPropList.pDrmFormatModifierProperties = modProps.data();
        ctx.physDev.getFormatProperties2(imgCI.format, &formatProps);

        for (auto& prop : modProps) {
            if (!filter_modifier(prop.drmFormatModifier)) {
                continue;
            }

            vk::PhysicalDeviceImageDrmFormatModifierInfoEXT modInfo {
                .drmFormatModifier = prop.drmFormatModifier,
                .sharingMode = imgCI.sharingMode,
                .queueFamilyIndexCount = imgCI.queueFamilyIndexCount,
                .pQueueFamilyIndices = imgCI.pQueueFamilyIndices,
            };
            vk::PhysicalDeviceImageFormatInfo2 formatInfo {
                .pNext = &modInfo,
                .format = imgCI.format,
                .type = imgCI.imageType,
                .tiling = imgCI.tiling,
                .usage = imgCI.usage,
                .flags = imgCI.flags,
            };
            vk::ImageFormatProperties2 imgFormatProps {
                .pNext = nullptr,
            };
            auto result = ctx.physDev.getImageFormatProperties2(&formatInfo, &imgFormatProps);
            if (result == vk::Result::eSuccess)
                imageModifiers.push_back(prop.drmFormatModifier);
        }

        extImgCI.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

        vk::ImageDrmFormatModifierListCreateInfoEXT modListCI {
            .pNext = &extImgCI,
            .drmFormatModifierCount = static_cast<u32>(imageModifiers.size()),
            .pDrmFormatModifiers = imageModifiers.data(),
        };

        imgCI.pNext = &modListCI;

        img.image = ctx.dev.createImage(imgCI);
    } else if (haveDmaBuf && handleType == HandleType::DmaBuf) {
        extImgCI.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
        imgCI.pNext = &extImgCI;
        imgCI.tiling = vk::ImageTiling::eLinear;

        img.image = ctx.dev.createImage(imgCI);
    } else if (handleType == HandleType::OpaqueFd) {
        extImgCI.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
        imgCI.pNext = &extImgCI;
        imgCI.tiling = vk::ImageTiling::eOptimal;

        img.image = ctx.dev.createImage(imgCI);
    } else if (handleType == HandleType::None) {
        imgCI.tiling = vk::ImageTiling::eOptimal;

        img.image = ctx.dev.createImage(imgCI);
    }

    vk::MemoryDedicatedRequirements dedicatedReqs {};
    vk::MemoryRequirements2 memReqs {
        .pNext = &dedicatedReqs,
    };

    vk::ImageMemoryRequirementsInfo2 memReqInfo {
        .image = img.image,
    };
    ctx.dev.getImageMemoryRequirements2(&memReqInfo, &memReqs);

    vk::MemoryDedicatedAllocateInfo memDedicatedInfo {
        .image = img.image,
    };
    vk::ExportMemoryAllocateInfo memExportInfo {
        .handleTypes = extImgCI.handleTypes,
    };
    if (handleType != HandleType::None)
        memDedicatedInfo.pNext = &memExportInfo;

    vk::MemoryAllocateInfo memAllocInfo {
        .pNext = &memDedicatedInfo,
        .allocationSize = memReqs.memoryRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(
            ctx, vk::MemoryPropertyFlagBits::eDeviceLocal, memReqs.memoryRequirements.memoryTypeBits
        ),
    };
    img.memory = ctx.dev.allocateMemory(memAllocInfo);

    vk::BindImageMemoryInfo bindImgInfo {
        .image = img.image,
        .memory = img.memory,
        .memoryOffset = 0,
    };
    ctx.dev.bindImageMemory2(bindImgInfo);

    // Only the dma-buf export produces a DRM descriptor; the opaque-fd path
    // leaves out.drm zeroed.
    if (handleType == HandleType::DmaBuf) {
        vk::MemoryGetFdInfoKHR memFdInfo {
            .memory = img.memory,
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        out.drm.fd = ctx.dev.getMemoryFdKHR(memFdInfo, ctx.dispatch);

        if (haveDrmModifiers) {
            auto imgDrmProps
                = ctx.dev.getImageDrmFormatModifierPropertiesEXT(img.image, ctx.dispatch);

            out.drm.modifier = imgDrmProps.drmFormatModifier;
            for (auto prop : modProps) {
                if (prop.drmFormatModifier == out.drm.modifier)
                    out.drm.planes = prop.drmFormatModifierPlaneCount;
            }
        } else {
            out.drm.modifier = DRM_FORMAT_MOD_INVALID;
            out.drm.planes = 1;
        }

        for (u32 i = 0; i < out.drm.planes; ++i) {
            vk::ImageSubresource subresource {
                .aspectMask = static_cast<vk::ImageAspectFlags>(
                    haveDrmModifiers ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i
                                     : VK_IMAGE_ASPECT_COLOR_BIT
                ),
            };
            auto layout = ctx.dev.getImageSubresourceLayout(img.image, subresource);
            out.drm.strides[i] = layout.rowPitch;
            out.drm.offsets[i] = layout.offset;
        }

        out.drm.format = to_drm_format(imgCI.format);
    }

    vk::ImageViewCreateInfo imgViewCI {
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = imgCI.format,
        .components {
            .r = vk::ComponentSwizzle::eIdentity,
            .g = vk::ComponentSwizzle::eIdentity,
            .b = vk::ComponentSwizzle::eIdentity,
            .a = vk::ComponentSwizzle::eIdentity,
        },
        .subresourceRange {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = imgCI.mipLevels,
            .baseArrayLayer = 0,
            .layerCount = imgCI.arrayLayers,
        },
    };
    img.view = ctx.dev.createImageView(imgViewCI);

    // out.semaphore = ctx.dev.createSemaphore({});

    out.imageCI = imgCI;
    out.size = memReqs.memoryRequirements.size;
    out.image = img;

    return out;
}
