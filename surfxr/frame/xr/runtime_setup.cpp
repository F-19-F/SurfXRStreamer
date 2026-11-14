#include "display_private.hpp"
#include "steamvr_refresh.hpp"
#include "surfxr/vulkan_dmabuf.hpp"

namespace surfxr {
void XrDisplay::Impl::create_xr() {
    std::uint32_t count = 0;
    xr_check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties(count)");
    if (count == 0 || count > 4096) throw std::runtime_error("Invalid OpenXR extension count");
    std::vector<XrExtensionProperties> extensions(count, xr_info<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    xr_check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties");
    bool found = false;
    for (const auto& item : extensions)
        found |= std::strcmp(item.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
    if (!found) throw std::runtime_error("Runtime lacks XR_KHR_vulkan_enable2");
    std::vector<const char*> enabled{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for (const auto& item: extensions) {
        if(std::strcmp(item.extensionName,XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)==0){
            enabled.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);stats_.eye_gaze_extension_enabled=true;
        }
        if (std::strcmp(item.extensionName,"XR_VALVE_frame_controller_interaction")==0) {
            enabled.push_back("XR_VALVE_frame_controller_interaction"); stats_.frame_controller_profile_enabled=true;
        }
        if(std::strcmp(item.extensionName,XR_EXT_INTERACTION_PROFILE_BATTERY_STATE_DISPLAY_EXTENSION_NAME)==0){
            enabled.push_back(XR_EXT_INTERACTION_PROFILE_BATTERY_STATE_DISPLAY_EXTENSION_NAME);
            stats_.battery_extension_enabled=true;
        }
        if (std::strcmp(item.extensionName,XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)==0) {
            enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);stats_.cylinder_layer_enabled=true;
        }
        if(std::strcmp(item.extensionName,XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)==0){
            enabled.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);stats_.timespec_time_extension_enabled=true;
        }
        if(std::strcmp(item.extensionName,XR_META_PERFORMANCE_METRICS_EXTENSION_NAME)==0){
            enabled.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);stats_.performance_metrics_extension_enabled=true;
        }
    }
    if(!boundary_requested_)for(const auto& item:extensions)if(std::strcmp(item.extensionName,XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)==0){enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);refresh_rate_extension_=true;}
    // SteamVR may expose display modes only through OpenVR, not XR_FB.
    // Probe before creating the XR instance so its own client lifetime is untouched.
    if(!refresh_rate_extension_&&!boundary_requested_)stats_.refresh_rates_hz=xr_detail::steamvr_refresh_rates();
    if(boundary_requested_)boundary_=std::make_unique<xr_detail::SteamVrBoundary>();
    auto info = xr_info<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strcpy(info.applicationInfo.applicationName, "SurfXR");
    std::strcpy(info.applicationInfo.engineName, "SurfXR");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    info.enabledExtensionNames = enabled.data();
    xr_check(xrCreateInstance(&info, &instance_), "xrCreateInstance");
    if(stats_.timespec_time_extension_enabled)
        timespec_to_xr_=extension<PFN_xrConvertTimespecTimeToTimeKHR>(instance_,"xrConvertTimespecTimeToTimeKHR");
    auto system_info = xr_info<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    xr_check(xrGetSystem(instance_, &system_info, &system_), "xrGetSystem");
    auto properties = xr_info<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    auto gaze_properties=xr_info<XrSystemEyeGazeInteractionPropertiesEXT>(XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT);
    if(stats_.eye_gaze_extension_enabled)properties.next=&gaze_properties;
    xr_check(xrGetSystemProperties(instance_, system_, &properties), "xrGetSystemProperties");
    stats_.eye_gaze_supported=stats_.eye_gaze_extension_enabled&&gaze_properties.supportsEyeGazeInteraction==XR_TRUE;
    max_width_ = properties.graphicsProperties.maxSwapchainImageWidth;
    max_height_ = properties.graphicsProperties.maxSwapchainImageHeight;
    max_layers_ = properties.graphicsProperties.maxLayerCount;
    stats_.runtime_layer_limit=max_layers_;
    if (!max_width_ || !max_height_ || properties.graphicsProperties.maxLayerCount < 1)
        throw std::runtime_error("OpenXR system cannot display a layer");
    xr_check(xrEnumerateEnvironmentBlendModes(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             0, &count, nullptr), "xrEnumerateEnvironmentBlendModes(count)");
    if (!count || count > 32) throw std::runtime_error("Invalid environment blend mode count");
    std::vector<XrEnvironmentBlendMode> modes(count);
    xr_check(xrEnumerateEnvironmentBlendModes(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             count, &count, modes.data()), "xrEnumerateEnvironmentBlendModes");
    if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
        throw std::runtime_error("This desktop viewer requires opaque environment blending");
}

void XrDisplay::Impl::create_vulkan() {
    const auto requirements_fn = extension<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance_, "xrGetVulkanGraphicsRequirements2KHR");
    const auto instance_fn = extension<PFN_xrCreateVulkanInstanceKHR>(instance_, "xrCreateVulkanInstanceKHR");
    const auto physical_fn = extension<PFN_xrGetVulkanGraphicsDevice2KHR>(instance_, "xrGetVulkanGraphicsDevice2KHR");
    const auto device_fn = extension<PFN_xrCreateVulkanDeviceKHR>(instance_, "xrCreateVulkanDeviceKHR");
    auto requirements = xr_info<XrGraphicsRequirementsVulkan2KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    xr_check(requirements_fn(instance_, system_, &requirements), "xrGetVulkanGraphicsRequirements2KHR");
    const XrVersion selected = std::max<XrVersion>(std::min<XrVersion>(XR_MAKE_VERSION(1, 3, 0),requirements.maxApiVersionSupported), requirements.minApiVersionSupported);
    if (selected > requirements.maxApiVersionSupported || XR_VERSION_MAJOR(selected) > 127 ||
        XR_VERSION_MINOR(selected) > 1023 || XR_VERSION_PATCH(selected) > 4095)
        throw std::runtime_error("No compatible Vulkan API version");
    const auto version = VK_MAKE_VERSION(XR_VERSION_MAJOR(selected), XR_VERSION_MINOR(selected), XR_VERSION_PATCH(selected));
    auto app = vk_info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "SurfXR";
    app.apiVersion = version;
    mip_format_list_enabled_=version>=VK_API_VERSION_1_2;
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
    max_image_ = properties.limits.maxImageDimension2D;
    stats_.max_render_width=std::min({max_width_,max_image_,properties.limits.maxFramebufferWidth,properties.limits.maxViewportDimensions[0]});
    stats_.max_render_height=std::min({max_height_,max_image_,properties.limits.maxFramebufferHeight,properties.limits.maxViewportDimensions[1]});
    if (properties.apiVersion < version)
        throw std::runtime_error("Vulkan physical device does not support the required API");
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
    VulkanDmaBufFeatures dmabuf;std::vector<const char*> device_extensions;
    dmabuf_device_enabled_=dmabuf.query(physical_);
    if(dmabuf_device_enabled_)dmabuf.attach(device,device_extensions);
    auto create_device = xr_info<XrVulkanDeviceCreateInfoKHR>(XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    create_device.systemId = system_;
    create_device.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create_device.vulkanPhysicalDevice = physical_;
    create_device.vulkanCreateInfo = &device;
    xr_check(device_fn(instance_, &create_device, &device_, &result), "xrCreateVulkanDeviceKHR");
    vk_check(result, "runtime vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

void XrDisplay::Impl::create_session() {
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
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    reference.poseInReferenceSpace.orientation.w = 1.0f;
    xr_check(xrCreateReferenceSpace(session_, &reference, &space_), "xrCreateReferenceSpace(LOCAL)");
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr_check(xrCreateReferenceSpace(session_, &reference, &head_space_), "xrCreateReferenceSpace(VIEW)");
    std::uint32_t reference_count=0;
    xr_check(xrEnumerateReferenceSpaces(session_,0,&reference_count,nullptr),"xrEnumerateReferenceSpaces(count)");
    if(reference_count>64)throw std::runtime_error("Invalid reference space count");
    std::vector<XrReferenceSpaceType> references(reference_count);
    if(reference_count)xr_check(xrEnumerateReferenceSpaces(session_,reference_count,&reference_count,references.data()),"xrEnumerateReferenceSpaces");
    if(std::find(references.begin(),references.end(),XR_REFERENCE_SPACE_TYPE_STAGE)!=references.end()){
        reference.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;
        xr_check(xrCreateReferenceSpace(session_,&reference,&stage_space_),"xrCreateReferenceSpace(STAGE)");
        stats_.stage_supported=true;refresh_stage_bounds();
    }
    if(refresh_rate_extension_){
        const auto enumerate=extension<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_,"xrEnumerateDisplayRefreshRatesFB");
        get_refresh_rate_=extension<PFN_xrGetDisplayRefreshRateFB>(instance_,"xrGetDisplayRefreshRateFB");
        request_refresh_rate_=extension<PFN_xrRequestDisplayRefreshRateFB>(instance_,"xrRequestDisplayRefreshRateFB");
        std::uint32_t n=0;xr_check(enumerate(session_,0,&n,nullptr),"xrEnumerateDisplayRefreshRatesFB(count)");
        if(n>128)throw std::runtime_error("Invalid display refresh rate count");
        std::vector<float> rates(n);if(n)xr_check(enumerate(session_,n,&n,rates.data()),"xrEnumerateDisplayRefreshRatesFB");
        for(float rate:rates)if(std::isfinite(rate)&&rate>0&&rate<=1000)stats_.refresh_rates_hz.push_back(rate);
        float rate=0;xr_check(get_refresh_rate_(session_,&rate),"xrGetDisplayRefreshRateFB");if(std::isfinite(rate)&&rate>0&&rate<=1000)stats_.current_refresh_hz=rate;
    }
    discover_performance_metrics();
    std::uint32_t count = 0;
    xr_check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats(count)");
    if (!count || count > 4096) throw std::runtime_error("Invalid swapchain format count");
    std::vector<std::int64_t> formats(count);
    xr_check(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()), "xrEnumerateSwapchainFormats");
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (const VkFormat candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_, candidate, &properties);
        constexpr auto required = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) != formats.end() &&
            (properties.optimalTilingFeatures & required) == required) { format = candidate; break; }
    }
    if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported RGBA/BGRA sRGB transfer-destination swapchain format");
    format_ = format;
    stats_.upload_format=static_cast<std::uint32_t>(format);
}

} // namespace surfxr
