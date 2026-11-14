#pragma once
#include "surfxr/image_enhancement.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <memory>

namespace surfxr {
struct ImageEnhancementEye {
    VkImage source=VK_NULL_HANDLE;
    VkImageView source_view=VK_NULL_HANDLE,target=VK_NULL_HANDLE;
    bool dirty=true;
    // Optional compatible UNORM views of these same source/target images.
    // They expose encoded sRGB values; callers must create mutable images.
    VkImageView raw_source_view=VK_NULL_HANDLE,raw_target=VK_NULL_HANDLE;
};
// Stateless spatial processing of complete E eyes into O; no history/poses.
// Inputs and outputs enter/leave COLOR_ATTACHMENT_OPTIMAL on the same queue.
// The input must be an sRGB view of an opaque reconstructed eye. Output is the
// same sRGB format, alpha=1. Distinct source/target images; no overlapping views.
// Two eyes share one submit and optional bounded fence; it also completes
// earlier same-queue foveation work. Caller owns acquire/release and all views.
// Cached target framebuffers must be destroyed before their views are freed.
class ImageEnhancementRenderer {
public:
    ImageEnhancementRenderer(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,
                             VkFormat format,unsigned source_width,unsigned source_height,
                             unsigned target_width,unsigned target_height);
    ~ImageEnhancementRenderer();
    ImageEnhancementRenderer(const ImageEnhancementRenderer&)=delete;
    ImageEnhancementRenderer& operator=(const ImageEnhancementRenderer&)=delete;
    // false = no GPU work; target is untouched. Callers use E in this case.
    bool render(const std::array<ImageEnhancementEye,2>& eyes,ImageEnhancementOptions options,bool defer_wait=false);
    // Retire before reusing command resources or destroying supplied views.
    // A later successful fence on this same queue may cover the submission.
    void complete(bool covered_by_later_render=false);
    double last_gpu_wait_ms()const noexcept;
    // Enhancement-only GPU span after source dependencies complete. Available
    // after complete(); -1 when unsupported/unavailable. Excludes foveation.
    double last_gpu_duration_ms()const noexcept;
    bool supports_gpu_timestamps()const noexcept;
    bool supports_raw_views()const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
