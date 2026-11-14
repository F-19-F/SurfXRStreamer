#pragma once
#include "dashboard_canvas.hpp"
#include "dashboard_keyboard.hpp"

namespace surfxr::dashboard {
enum class UploadResult { published, retry, reconnect };

// The currently displayed source image is never an upload destination. A failed
// publication keeps both the current texture and its slot unchanged.


struct TexturePublication {
  int front = -1;
  unsigned candidate() const { return front == 0 ? 1u : 0u; }
  UploadResult complete(vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) {
      front = static_cast<int>(candidate());
      return UploadResult::published;
    }
    if (error == vr::VROverlayError_InvalidHandle || error == vr::VROverlayError_UnknownOverlay)
      return UploadResult::reconnect;
    return UploadResult::retry;
  }
};

template<class T> T vk_info(VkStructureType type) { T value{}; value.sType = type; return value; }
class VulkanFailure : public std::runtime_error {
 public:
  VulkanFailure(VkResult result, const char* operation)
      : std::runtime_error(std::string(operation) + ": " + std::to_string(result)), result(result) {}
  VkResult result;
};
inline void vk_check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw VulkanFailure(result, operation);
}

// SteamVR schedules its texture copy on this queue. Keep the device, images and
// VRVulkanTextureData_t alive until after VR_Shutdown, and synchronize queue use
// before overwriting any staging memory or reusing an old source image.
class DashboardTextures {
 public:
  DashboardTextures() = default;
  DashboardTextures(const DashboardTextures&) = delete;
  DashboardTextures& operator=(const DashboardTextures&) = delete;
  ~DashboardTextures() {
    if (device_) {
      (void)vkDeviceWaitIdle(device_);
      if (mapped_) vkUnmapMemory(device_, staging_memory_);
      if (fence_) vkDestroyFence(device_, fence_, nullptr);
      if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
      if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
      if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
      for (auto& slot : images_) {
        if (slot.image) vkDestroyImage(device_, slot.image, nullptr);
        if (slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
      }
      vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
  }
  void initialize() {
    auto* compositor = vr::VRCompositor();
    auto* system = vr::VRSystem();
    if (!compositor || !system) throw std::runtime_error("SteamVR texture sharing is unavailable.");
    const auto instance_names = extensions([compositor](char* data, uint32_t size) {
      return compositor->GetVulkanInstanceExtensionsRequired(data, size);
    });
    const auto instance_ptrs = pointers(instance_names);
    auto app = vk_info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "SurfXR dashboard";
    app.apiVersion = VK_API_VERSION_1_1;
    auto instance_info = vk_info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_ptrs.size());
    instance_info.ppEnabledExtensionNames = instance_ptrs.data();
    vk_check(vkCreateInstance(&instance_info, nullptr, &instance_), "Create dashboard Vulkan instance");
    uint64_t output = 0;
    system->GetOutputDevice(&output, vr::TextureType_Vulkan, instance_);
    physical_ = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(output));
    if (!physical_) throw std::runtime_error("SteamVR did not identify its graphics device.");
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties_);
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
    auto family = std::find_if(families.begin(), families.end(), [](const auto& f) {
      return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT);
    });
    if (family == families.end()) throw std::runtime_error("No dashboard graphics queue is available.");
    family_ = static_cast<uint32_t>(family - families.begin());
    const auto device_names = extensions([compositor, this](char* data, uint32_t size) {
      return compositor->GetVulkanDeviceExtensionsRequired(physical_, data, size);
    });
    const auto device_ptrs = pointers(device_names);
    const float priority = 0.0f;
    auto queue_info = vk_info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue_info.queueFamilyIndex = family_; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
    auto device_info = vk_info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_ptrs.size());
    device_info.ppEnabledExtensionNames = device_ptrs.data();
    vk_check(vkCreateDevice(physical_, &device_info, nullptr, &device_), "Create dashboard Vulkan device");
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    for (unsigned i = 0; i != images_.size(); ++i)
      create_image(images_[i], i == 2 ? 128u : width, i == 2 ? 128u : height);
    auto buffer_info = vk_info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer_info.size = static_cast<VkDeviceSize>(width) * height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vk_check(vkCreateBuffer(device_, &buffer_info, nullptr, &staging_), "Create dashboard upload buffer");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device_, staging_, &req);
    auto alloc = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    coherent_ = (memory_properties_.memoryTypes[alloc.memoryTypeIndex].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    vk_check(vkAllocateMemory(device_, &alloc, nullptr, &staging_memory_), "Allocate dashboard upload memory");
    vk_check(vkBindBufferMemory(device_, staging_, staging_memory_, 0), "Bind dashboard upload memory");
    vk_check(vkMapMemory(device_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "Map dashboard upload memory");
    auto pool_info = vk_info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool_info.queueFamilyIndex = family_; pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_), "Create dashboard command pool");
    auto command_info = vk_info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    command_info.commandPool = pool_; command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(device_, &command_info, &command_), "Create dashboard command buffer");
    auto fence_info = vk_info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    vk_check(vkCreateFence(device_, &fence_info, nullptr, &fence_), "Create dashboard upload fence");
  }
  vr::Texture_t upload(unsigned index, Canvas& canvas) {
    auto& slot = images_.at(index);
    // Includes the previous SetOverlayTexture copy, issued by SteamVR on our
    // queue. Nothing in this process accesses this queue from another thread.
    vk_check(vkQueueWaitIdle(queue_), "Wait for dashboard texture copy");
    std::memcpy(mapped_, canvas.pixels(), static_cast<size_t>(slot.data.m_nWidth) * slot.data.m_nHeight * 4);
    if (!coherent_) {
      auto range = vk_info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
      range.memory = staging_memory_; range.size = VK_WHOLE_SIZE;
      vk_check(vkFlushMappedMemoryRanges(device_, 1, &range), "Flush dashboard upload memory");
    }
    vk_check(vkResetCommandBuffer(command_, 0), "Reset dashboard command buffer");
    vk_check(vkResetFences(device_, 1, &fence_), "Reset dashboard upload fence");
    auto begin = vk_info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command_, &begin), "Begin dashboard texture copy");
    auto barrier = vk_info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = slot.image; barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = slot.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = slot.initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, slot.initialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {slot.data.m_nWidth, slot.data.m_nHeight, 1};
    vkCmdCopyBufferToImage(command_, staging_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vk_check(vkEndCommandBuffer(command_), "Finish dashboard texture copy");
    auto submit = vk_info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    vk_check(vkQueueSubmit(queue_, 1, &submit, fence_), "Submit dashboard texture copy");
    // The final layout is now scheduled even if the bounded wait times out.
    // A retry first waits for this queue before reusing either buffer or image.
    slot.initialized = true;
    vk_check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1'000'000'000), "Complete dashboard texture copy");
    return {&slot.data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
  }
 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    vr::VRVulkanTextureData_t data{};
    bool initialized = false;
  };
  template<class Query> static std::vector<std::string> extensions(Query query) {
    const uint32_t size = query(nullptr, 0);
    if (!size) return {};
    if (size > 65536) throw std::runtime_error("The SteamVR Vulkan extension list is invalid.");
    std::vector<char> buffer(size, '\0');
    if (query(buffer.data(), size) > size || buffer.back() != '\0')
      throw std::runtime_error("The SteamVR Vulkan extension list changed.");
    std::istringstream stream(buffer.data());
    std::vector<std::string> names;
    for (std::string name; stream >> name;) names.push_back(std::move(name));
    return names;
  }
  static std::vector<const char*> pointers(const std::vector<std::string>& names) {
    std::vector<const char*> result;
    for (const auto& name : names) result.push_back(name.c_str());
    return result;
  }
  uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
    for (unsigned pass = 0; pass != 2; ++pass) {
      const auto flags = required | (pass == 0 ? preferred : 0);
      for (uint32_t i = 0; i != memory_properties_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_properties_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("No suitable dashboard graphics memory is available.");
  }
  void create_image(Image& slot, uint32_t w, uint32_t h) {
    auto info = vk_info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1}; info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vk_check(vkCreateImage(device_, &info, nullptr, &slot.image), "Create dashboard texture");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device_, slot.image, &req);
    auto alloc = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device_, &alloc, nullptr, &slot.memory), "Allocate dashboard texture memory");
    vk_check(vkBindImageMemory(device_, slot.image, slot.memory, 0), "Bind dashboard texture memory");
    // VkImage is an opaque 64-bit handle on both supported Vulkan handle ABIs.
    static_assert(sizeof(slot.image) == sizeof(slot.data.m_nImage), "Unexpected Vulkan image handle size");
    std::memcpy(&slot.data.m_nImage, &slot.image, sizeof(slot.image));
    slot.data.m_pDevice = device_; slot.data.m_pPhysicalDevice = physical_;
    slot.data.m_pInstance = instance_; slot.data.m_pQueue = queue_; slot.data.m_nQueueFamilyIndex = family_;
    slot.data.m_nWidth = w; slot.data.m_nHeight = h; slot.data.m_nFormat = info.format; slot.data.m_nSampleCount = 1;
  }
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  std::array<Image, 3> images_{};
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  void* mapped_ = nullptr;
  bool coherent_ = false;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
};

class Dashboard {
 public:
  Dashboard() = default;
  ~Dashboard() { disconnect(); }
  bool connected() const { return api_ != nullptr; }
  bool connect() {
    if (connected()) return true;
    vr::EVRInitError error = vr::VRInitError_None;
    // Background does not launch vrserver. Only attach as an overlay after a
    // successful probe; the native Steam launcher remains the runtime owner.
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) return false;
    vr::VR_Shutdown();
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) return false;
    initialized_ = true;
    api_ = vr::VROverlay();
    if (!api_) { disconnect(); return false; }
    try {
      textures_ = std::make_unique<DashboardTextures>();
      textures_->initialize();
    } catch (const std::exception& e) {
      warn(e.what()); disconnect(); return false;
    }
    if (api_->CreateDashboardOverlay(overlay_key, "SurfXR", &main_, &thumbnail_) != vr::VROverlayError_None) {
      disconnect(); return false;
    }
    const vr::HmdVector2_t mouse_scale{{static_cast<float>(width), static_cast<float>(height)}};
    if (api_->SetOverlayWidthInMeters(main_, 2.0f) != vr::VROverlayError_None ||
        api_->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse) != vr::VROverlayError_None ||
        api_->SetOverlayMouseScale(main_, &mouse_scale) != vr::VROverlayError_None) {
      disconnect(); return false;
    }
    Canvas icon(128, 128);
    color(icon.context(), {0.05, 0.09, 0.13}); cairo_paint(icon.context());
    monitor(icon.context(), 22, 26, 1.45, mint);
    try {
      const auto texture = textures_->upload(2, icon);
      if (api_->SetOverlayTexture(thumbnail_, &texture) != vr::VROverlayError_None) {
        warn("SteamVR did not accept the dashboard icon texture.");
        disconnect(); return false;
      }
    } catch (const std::exception& e) {
      warn(e.what()); disconnect(); return false;
    }
    return true;
  }
  void disconnect() {
    if (api_) {
      host_keyboard_.cancel(api_);
      if (main_ != vr::k_ulOverlayHandleInvalid) api_->DestroyOverlay(main_);
      // The thumbnail belongs to the dashboard overlay and cannot be destroyed
      // independently. VR_Shutdown also releases any partially created handles.
    }
    main_ = thumbnail_ = vr::k_ulOverlayHandleInvalid;
    api_ = nullptr;
    if (initialized_) vr::VR_Shutdown();
    initialized_ = false;
    // OpenVR may retain the images/device; it must detach before destruction.
    textures_.reset();
    publication_ = {};
  }
  bool visible() const { return api_ && api_->IsOverlayVisible(main_); }
  void show() { if (api_) api_->ShowDashboard(overlay_key); }
  bool event(vr::VREvent_t& event) { return api_ && api_->PollNextOverlayEvent(main_, &event, sizeof(event)); }
  bool edit_host_entry(bool host=false) { return host_keyboard_.open(api_, main_,host); }
  bool accept_host_entry(const vr::VREvent_t& event, std::string& host_entry) {
    return host_keyboard_.accept(api_, event, host_entry);
  }
  void cancel_host_entry() { host_keyboard_.cancel(api_); }
  bool runtime_quit() {
    if (!api_ || !vr::VRSystem()) return false;
    vr::VREvent_t e{};
    for (unsigned count = 0; count != 64 && vr::VRSystem()->PollNextEvent(&e, sizeof(e)); ++count) {
      if (e.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        return true;
      }
    }
    return false;
  }
  UploadResult upload(Canvas& canvas) {
    if (!api_ || !textures_) return UploadResult::reconnect;
    try {
      const auto texture = textures_->upload(publication_.candidate(), canvas);
      const auto error = api_->SetOverlayTexture(main_, &texture);
      const auto result = publication_.complete(error);
      if (result == UploadResult::published) {
        last_warning_.clear();
      } else {
        warn("SteamVR texture update deferred: " + std::to_string(error));
      }
      return result;
    } catch (const VulkanFailure& e) {
      warn(e.what());
      return e.result == VK_ERROR_DEVICE_LOST ? UploadResult::reconnect : UploadResult::retry;
    } catch (const std::exception& e) {
      warn(e.what());
      return UploadResult::retry;
    }
  }
 private:
  void warn(const std::string& message) {
    if (last_warning_ != message) std::cerr << "surfxr-dashboard: " << message << '\n';
    last_warning_ = message;
  }
  bool initialized_ = false;
  HostKeyboard<vr::IVROverlay> host_keyboard_;
  std::unique_ptr<DashboardTextures> textures_;
  TexturePublication publication_;
  std::string last_warning_;
  vr::IVROverlay* api_ = nullptr;
  vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid;
  vr::VROverlayHandle_t thumbnail_ = vr::k_ulOverlayHandleInvalid;
};


}
