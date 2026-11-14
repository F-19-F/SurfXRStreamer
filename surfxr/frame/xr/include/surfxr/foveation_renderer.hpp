#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <memory>

namespace surfxr {
// EncodedSrgb8 retains the original quantized sRGB reference. Gamma2 sharpens
// in a square-root perceptual domain and writes linear color to an sRGB target.
enum class FoveationSharpening {EncodedSrgb8,Gamma2};
struct FoveationEye {
    VkImage source=VK_NULL_HANDLE;
    VkImageView source_view=VK_NULL_HANDLE,target=VK_NULL_HANDLE;
    bool dirty=true;
    // Optional compatible UNORM view, only for EncodedSrgb8 fused sharpening.
    // The image must be mutable; its stored color remains sRGB encoded.
    VkImageView raw_target=VK_NULL_HANDLE;
    // Optional CPU-mapped linear source. It enters PREINITIALIZED on first
    // use and otherwise GENERAL, and remains GENERAL after reconstruction.
    // Caller retains its complete immutable frame through the render fence.
    bool host_source=false,host_source_initialized=false;
    // Imported DMA-BUF is acquired from and released to FOREIGN every submit.
    bool foreign_source=false;
    VkImage focus_source=VK_NULL_HANDLE;
    VkImageView focus_view=VK_NULL_HANDLE;
    bool focus_host_source=false,focus_host_initialized=false;
    // Complete-eye pixel rect from this frame's metadata, never latest gaze.
    std::array<float,4> focus_rect{};
    bool linear_layout=false;
    // Normalized packed source region; clamp each eye independently at its
    // edges. Shared host images are transitioned once per submission.
    std::array<float,4> source_rect{0,0,1,1};
    std::array<float,4> focus_source_rect{0,0,1,1};
    VkImage chroma_source=VK_NULL_HANDLE;
    VkImageView chroma_view=VK_NULL_HANDLE;
    std::array<float,4> yuv_coefficients{};
    // Desktop's existing two-pixel 64/192 alpha feather, default off for VR.
    bool desktop_border=false;
    // Optional rotational timewarp before nonlinear/gaze sampling. Zero FOV
    // disables it. Quaternion maps destination rays into source-eye space.
    std::array<float,4> timewarp_rotation{0,0,0,1};
    std::array<float,4> timewarp_fov{}; // positive left/right/up/down tangents
};
// Independent Vulkan full-screen reconstruction, no XR or external state.
// Normal inputs and outputs enter/leave COLOR_ATTACHMENT_OPTIMAL; host_source
// inputs use GENERAL as described above. Both eyes use
// one queue submission and an optional bounded fence wait. Target views must outlive this
// object; renderer owns cached framebuffers but never supplied images/views.
class FoveationRenderer {
public:
    FoveationRenderer(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                     unsigned family,VkFormat format,unsigned width,unsigned height,bool allow_quad=false,
                     bool zero_weight_skip=true,FoveationSharpening sharpening=FoveationSharpening::EncodedSrgb8);
    ~FoveationRenderer();
    FoveationRenderer(const FoveationRenderer&)=delete;
    FoveationRenderer& operator=(const FoveationRenderer&)=delete;
    // Positive sharpness fuses native-size sharpening into reconstruction.
    // Samples the complete-eye pixel grid in the selected sharpening domain.
    // Gamma2 requires an sRGB target and rejects a raw_target when sharpening.
    // Zero retains the original reconstruction shader path and sRGB target.
    bool render(const std::array<FoveationEye,2>& eyes,bool defer_wait=false,float sharpness=0);
    // A later successful fence on the same queue can cover this submission.
    // Call before host access/reuse or destruction of supplied resources.
    // XR_KHR_vulkan_enable2 permits releasing submitted images to its bound
    // queue before execution completes; callers must still protect CPU storage.
    void complete(bool covered_by_later_render=false);
    double last_gpu_wait_ms() const noexcept;
    // GPU timestamp span of reconstruction and source barriers, distinct from
    // CPU fence wait. Available after complete(); -1 when unsupported/unavailable.
    double last_gpu_duration_ms() const noexcept;
    bool supports_gpu_timestamps() const noexcept;
    bool supports_raw_views() const noexcept;
    // allow_quad requires a Vulkan 1.1+ caller. The renderer still checks
    // fragment subgroup support and otherwise keeps the portable shader.
    bool supports_quad_sharpening() const noexcept;
    // Skip remaining neighborhood work only when known samples prove the
    // sharpening amplitude is zero. False retains the full reference path.
    bool zero_weight_skip_enabled() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
