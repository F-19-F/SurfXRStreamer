#include "presenter_private.hpp"
namespace surfxr::diagnostics {
void Presenter::create_xr() {
    std::uint32_t count = 0;
    xr_check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties(count)");
    if (count == 0 || count > 4096) throw std::runtime_error("Invalid OpenXR extension count");
    std::vector<XrExtensionProperties> extensions(count, xr_info<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    xr_check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties");
    bool found = false;
    for (const auto& item : extensions)
        found |= std::strcmp(item.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
    if (!found) throw std::runtime_error("Runtime lacks XR_KHR_vulkan_enable2");
    const char* enabled[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    auto info = xr_info<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strcpy(info.applicationInfo.applicationName, "frame_rgba_presenter");
    std::strcpy(info.applicationInfo.engineName, "SurfXR diagnostics");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
    xr_check(xrCreateInstance(&info, &instance_), "xrCreateInstance");
    auto system_info = xr_info<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    xr_check(xrGetSystem(instance_, &system_info, &system_), "xrGetSystem");
    auto properties = xr_info<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    xr_check(xrGetSystemProperties(instance_, system_, &properties), "xrGetSystemProperties");
    if (options_.width > properties.graphicsProperties.maxSwapchainImageWidth ||
        options_.height > properties.graphicsProperties.maxSwapchainImageHeight ||
        properties.graphicsProperties.maxLayerCount < 1)
        throw std::runtime_error("Input dimensions exceed OpenXR system limits");
    xr_check(xrEnumerateEnvironmentBlendModes(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             0, &count, nullptr), "xrEnumerateEnvironmentBlendModes(count)");
    if (!count || count > 32) throw std::runtime_error("Invalid environment blend mode count");
    std::vector<XrEnvironmentBlendMode> modes(count);
    xr_check(xrEnumerateEnvironmentBlendModes(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             count, &count, modes.data()), "xrEnumerateEnvironmentBlendModes");
    if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
        throw std::runtime_error("This desktop viewer requires opaque environment blending");
}

void Presenter::create_vulkan() {
    const auto requirements_fn = extension<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance_, "xrGetVulkanGraphicsRequirements2KHR");
    const auto instance_fn = extension<PFN_xrCreateVulkanInstanceKHR>(instance_, "xrCreateVulkanInstanceKHR");
    const auto physical_fn = extension<PFN_xrGetVulkanGraphicsDevice2KHR>(instance_, "xrGetVulkanGraphicsDevice2KHR");
    const auto device_fn = extension<PFN_xrCreateVulkanDeviceKHR>(instance_, "xrCreateVulkanDeviceKHR");
    auto requirements = xr_info<XrGraphicsRequirementsVulkan2KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    xr_check(requirements_fn(instance_, system_, &requirements), "xrGetVulkanGraphicsRequirements2KHR");
    const XrVersion selected = std::max<XrVersion>(XR_MAKE_VERSION(1, 1, 0), requirements.minApiVersionSupported);
    if (selected > requirements.maxApiVersionSupported || XR_VERSION_MAJOR(selected) > 127 ||
        XR_VERSION_MINOR(selected) > 1023 || XR_VERSION_PATCH(selected) > 4095)
        throw std::runtime_error("No compatible Vulkan API version");
    const auto version = VK_MAKE_VERSION(XR_VERSION_MAJOR(selected), XR_VERSION_MINOR(selected), XR_VERSION_PATCH(selected));
    auto app = vk_info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "frame_rgba_presenter";
    app.apiVersion = version;
    auto instance_info = vk_info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    instance_info.pApplicationInfo = &app;
    auto create = xr_info<XrVulkanInstanceCreateInfoKHR>(XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    create.systemId = system_;
    create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create.vulkanCreateInfo = &instance_info;
    VkResult result = VK_SUCCESS;
    xr_check(instance_fn(instance_, &create, &vk_instance_, &result), "xrCreateVulkanInstanceKHR");
    vk_check(result, "runtime vkCreateInstance");
    auto get_device = xr_info<XrVulkanGraphicsDeviceGetInfoKHR>(XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    get_device.systemId = system_;
    get_device.vulkanInstance = vk_instance_;
    xr_check(physical_fn(instance_, &get_device, &physical_), "xrGetVulkanGraphicsDevice2KHR");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    if (properties.apiVersion < version || options_.width > properties.limits.maxImageDimension2D ||
        options_.height > properties.limits.maxImageDimension2D)
        throw std::runtime_error("Vulkan physical device limits do not support this input/API");
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    if (!count || count > 4096) throw std::runtime_error("Invalid Vulkan queue family count");
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            queue_family_ = i; found = true; break;
        }
    }
    if (!found) throw std::runtime_error("No graphics-capable Vulkan queue");
    const float priority = 1.0f;
    auto queue = vk_info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    auto device = vk_info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    auto create_device = xr_info<XrVulkanDeviceCreateInfoKHR>(XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    create_device.systemId = system_;
    create_device.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create_device.vulkanPhysicalDevice = physical_;
    create_device.vulkanCreateInfo = &device;
    xr_check(device_fn(instance_, &create_device, &device_, &result), "xrCreateVulkanDeviceKHR");
    vk_check(result, "runtime vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

void Presenter::create_session(std::vector<std::uint8_t>& rgba) {
    auto binding = xr_info<XrGraphicsBindingVulkan2KHR>(XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
    binding.instance = vk_instance_;
    binding.physicalDevice = physical_;
    binding.device = device_;
    binding.queueFamilyIndex = queue_family_;
    binding.queueIndex = 0;
    auto session_info = xr_info<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    session_info.next = &binding;
    session_info.systemId = system_;
    xr_check(xrCreateSession(instance_, &session_info, &session_), "xrCreateSession");
    auto reference = xr_info<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    reference.poseInReferenceSpace.orientation.w = 1.0f;
    xr_check(xrCreateReferenceSpace(session_, &reference, &space_), "xrCreateReferenceSpace(VIEW)");
    std::uint32_t count = 0;
    xr_check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats(count)");
    if (!count || count > 4096) throw std::runtime_error("Invalid swapchain format count");
    std::vector<std::int64_t> formats(count);
    xr_check(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()), "xrEnumerateSwapchainFormats");
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (const VkFormat candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_, candidate, &properties);
        constexpr auto required = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) != formats.end() &&
            (properties.optimalTilingFeatures & required) == required) { format = candidate; break; }
    }
    if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported RGBA/BGRA sRGB transfer-destination swapchain format");
    if (format == VK_FORMAT_B8G8R8A8_SRGB)
        for (std::size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    auto info = xr_info<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = options_.width;
    info.height = options_.height;
    info.faceCount = info.arraySize = info.mipCount = 1;
    xr_check(xrCreateSwapchain(session_, &info, &swapchain_), "xrCreateSwapchain");
    xr_check(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr), "xrEnumerateSwapchainImages(count)");
    if (!count || count > 64) throw std::runtime_error("Invalid swapchain image count");
    images_.assign(count, xr_info<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
    xr_check(xrEnumerateSwapchainImages(swapchain_, count, &count,
             reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())), "xrEnumerateSwapchainImages");
    images_.resize(count);
}

void Presenter::create_transfer(const std::vector<std::uint8_t>& rgba) {
    auto buffer = vk_info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer.size = rgba.size();
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(device_, &buffer, nullptr, &buffer_), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    std::uint32_t index = UINT32_MAX;
    bool coherent = false;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1U << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            index = i;
            coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            if (coherent) break;
        }
    }
    if (index == UINT32_MAX) throw std::runtime_error("No host-visible Vulkan staging memory");
    auto allocation = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = index;
    vk_check(vkAllocateMemory(device_, &allocation, nullptr, &memory_), "vkAllocateMemory");
    vk_check(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
    vk_check(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
    std::memcpy(mapped_, rgba.data(), rgba.size());
    if (!coherent) {
        auto range = vk_info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = memory_;
        range.size = VK_WHOLE_SIZE;
        vk_check(vkFlushMappedMemoryRanges(device_, 1, &range), "vkFlushMappedMemoryRanges");
    }
    vkUnmapMemory(device_, memory_);
    mapped_ = nullptr;
    auto pool = vk_info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    vk_check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    auto commands = vk_info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    commands.commandPool = pool_;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_, &commands, &command_), "vkAllocateCommandBuffers");
    auto fence = vk_info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    vk_check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
}

void Presenter::copy_image(std::uint32_t index) {
    if (index >= images_.size()) throw std::runtime_error("Runtime returned an out-of-bounds swapchain image index");
    vk_check(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer");
    vk_check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    auto begin = vk_info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
    // OpenXR supplies and expects COLOR_ATTACHMENT_OPTIMAL on this queue.
    auto barrier = vk_info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images_[index].image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {options_.width, options_.height, 1};
    vkCmdCopyBufferToImage(command_, buffer_, images_[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vk_check(vkEndCommandBuffer(command_), "vkEndCommandBuffer");
    auto submit = vk_info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    vk_check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
    // Static-frame diagnostic favors explicit completion and simple ownership.
    vk_check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 2'000'000'000ULL), "vkWaitForFences(2s)");
}

}
