#pragma once
#include "surfxr/vr_ui.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace surfxr {
// Column-major Vulkan clip matrix: right-handed -Z view, Y flipped once,
// depth [0,1]. FOV order is left, right, up, down (radians).
std::array<float,16> projection_view_matrix(UiPose eye,const std::array<float,4>& fov,
                                           float near_z=.05f,float far_z=100.f);
struct ProjectionVertex {float position[3];float uv[2];float normal[3];};
std::vector<ProjectionVertex> projection_panel_mesh(const UiPanel& panel,unsigned segments=64);
struct ProjectionSource {
    VkImage image=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    UiPanel panel;
    // QualitySS: RGBA/BGRA sRGB, or NV12. Baseline NV12 caches only mip0.
    // The slot names a monitor, not a swapchain image. Content generation must
    // change for new pixels, including same-size source recreation. A repeated
    // generation is cached independently of rotating XR image/view handles.
    unsigned width=0,height=0,cache_slot=0;
    std::uint64_t content_generation=0;
    // Optional NV12 planes for direct cache reconstruction. Inputs
    // enter/leave GENERAL (FOREIGN ownership for imported DMA-BUF). Caller
    // retains the complete source/lease through this synchronous render fence.
    VkImage chroma_image=VK_NULL_HANDLE;
    VkImageView chroma_view=VK_NULL_HANDLE;
    std::array<float,4> source_rect{0,0,1,1},yuv_coefficients{};
    bool nv12_initialized=false,nv12_foreign=false;
};
enum class ProjectionSampling : unsigned { Baseline=0,QualitySS=1 };
struct ProjectionSamplingStats {
    std::uint64_t baseline_frames=0,quality_frames=0,mip_updates=0,mip_reuses=0;
    std::uint64_t cached_bytes=0,cached_levels=0;
    // Logical RGBA pixel payload removed, not measured DRAM traffic.
    std::uint64_t nv12_encoded_mip_updates=0,nv12_mip_updates=0,nv12_intermediate_bytes_avoided=0;
    std::uint64_t nv12_base_updates=0,nv12_base_reuses=0,nv12_encoded_base_updates=0;
    bool gpu_timestamps=false;
    // Samples count successful sampled renders, not every frame or monitor.
    // Mip timings include all dirty sources; projection covers both eyes.
    // Diagnostic samples drain the GPU pipeline at stage boundaries; they
    // are not zero-overhead observations. The source/upload tail is excluded.
    std::uint64_t mip_gpu_samples=0,projection_gpu_samples=0;
    double mip_gpu_ms_sum=0,mip_gpu_ms_max=0,projection_gpu_ms_sum=0,projection_gpu_ms_max=0;
    // -1 means not sampled/unsupported/unavailable. With valid timing but no dirty source,
    // last_mip_gpu_ms is zero and mip_gpu_samples does not increase.
    double last_mip_gpu_ms=-1,last_projection_gpu_ms=-1;
};
struct ProjectionEye {
    VkImageView target=VK_NULL_HANDLE; // single-layer, sample-count 1, selected sRGB format
    UiPose pose;
    std::array<float,4> fov{};
};
// No XR calls or ownership of supplied images/views. Synchronous queue use:
// RGBA sources enter/leave COLOR_ATTACHMENT_OPTIMAL; NV12 uses the contract
// above. Target images finish in
// COLOR_ATTACHMENT_OPTIMAL. Both eyes are rendered and fenced in one submission.
// image_format_list requires caller-enabled Vulkan 1.2 or KHR_image_format_list;
// restricts mutable NV12 mip views to sRGB/UNORM so the driver can optimize.
// False retains sRGB NV12 conversion without a mutable mip allocation.
// Targets must retain their image views until this renderer is destroyed.
// max_samples is 1, 2 or 4; selects the largest supported count <= this limit.
// timestamp_period=0 disables diagnostic timestamps and their execution
// boundaries. Otherwise sample the first render and every Nth render after it.
class ProjectionRenderer {
public:
    ProjectionRenderer(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,
                       VkFormat format,unsigned width,unsigned height,unsigned max_samples=4,
                       unsigned timestamp_period=0,bool image_format_list=false);
    ~ProjectionRenderer();
    ProjectionRenderer(const ProjectionRenderer&)=delete;
    ProjectionRenderer& operator=(const ProjectionRenderer&)=delete;
    // display_frame is retained for call-site compatibility; stable filtering
    // does not depend on it. Filtering is in the application projection pass,
    // before the runtime's final distortion/reprojection.
    void render(const std::vector<ProjectionSource>& sources,const std::array<ProjectionEye,2>& eyes,
                ProjectionSampling sampling=ProjectionSampling::Baseline,std::uint64_t display_frame=0);
    // Read-only cache preflight, including sampling mode (premultiplication
    // and mip allocation differ). Baseline RGBA still reads its supplied image
    // and must not use this to skip uploads; only Baseline NV12 owns a cache.
    // Zero extent/generation returns false; an invalid slot throws as invalidate
    // does. This does not allocate, invalidate, or change sampling statistics.
    bool can_reuse_source(unsigned cache_slot,unsigned width,unsigned height,
                          std::uint64_t content_generation,ProjectionSampling sampling=ProjectionSampling::QualitySS) const;
    void invalidate(unsigned cache_slot);
    ProjectionSamplingStats statistics() const noexcept;
    unsigned samples() const noexcept;
    double last_gpu_wait_ms() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
