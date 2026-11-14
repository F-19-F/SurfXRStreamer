#pragma once
#include "surfxr/image_enhancement.hpp"

namespace surfxr {
enum class PostprocessColorDomain {SrgbCodeValues,Gamma2};
enum class PostprocessVariant {Bypass,Sharpen,Upscale,UpscaleSharpen};
// Future point transforms (for example gamma) belong after spatial processing
// in its declared color domain, before the attachment transfer. Fuse compatible
// transforms into that shader tail; do not move neighborhood filters across
// the encoded-D to complete-E geometry reconstruction. No user gamma control exists.
enum class PostprocessPointStage {AfterSpatialBeforeAttachmentTransfer};
struct PostprocessPlan {
    ImageEnhancementExtent decoded,complete,output;
    ImageEnhancementOptions options; // Effective options, after sizing/limits.
    bool enhancement_active=false,upscale_active=false,fused_sharpen=false;
    unsigned pass_count=1; // Required D -> E pass, plus at most one E -> O pass.
    PostprocessColorDomain color_domain=PostprocessColorDomain::SrgbCodeValues;
    PostprocessVariant variant=PostprocessVariant::Bypass;
    PostprocessPointStage point_stage=PostprocessPointStage::AfterSpatialBeforeAttachmentTransfer;
};
// Pure sizing/policy only: no Vulkan handles, ownership, allocation or waits.
// D and E stay distinct even when their dimensions happen to match. Image view
// choice is an execution detail: sRGB filtering decodes before interpolation;
// an available raw view can fetch stored code values without a transfer roundtrip.
inline PostprocessPlan make_postprocess_plan(unsigned decoded_width,unsigned decoded_height,
                                             unsigned complete_width,unsigned complete_height,
                                             ImageEnhancementOptions options,
                                             unsigned max_width,unsigned max_height){
    if(!valid_image_enhancement_options(options)||!decoded_width||!decoded_height||
       !complete_width||!complete_height||!max_width||!max_height||
       decoded_width>max_width||decoded_height>max_height||
       complete_width>max_width||complete_height>max_height)
        throw std::invalid_argument("Invalid postprocess input or limits");
    PostprocessPlan plan;
    plan.decoded={decoded_width,decoded_height,1,false};
    plan.complete={complete_width,complete_height,1,false};
    plan.output=image_enhancement_extent(complete_width,complete_height,options.super_resolution);
    if(plan.output.width>max_width||plan.output.height>max_height)plan.output=plan.complete;
    plan.upscale_active=plan.output.upscale;
    plan.options={plan.upscale_active,options.sharpness};
    plan.enhancement_active=plan.upscale_active||options.sharpness>0;
    // A same-size sharpen reconstructs its complete-E neighbors directly in
    // the Gamma2 perceptual domain. Upscaling keeps the sRGB code-value filter.
    plan.fused_sharpen=!plan.upscale_active&&options.sharpness>0;
    plan.color_domain=plan.fused_sharpen?PostprocessColorDomain::Gamma2:PostprocessColorDomain::SrgbCodeValues;
    plan.pass_count=plan.upscale_active?2u:1u;
    plan.variant=plan.upscale_active?(options.sharpness>0?PostprocessVariant::UpscaleSharpen:PostprocessVariant::Upscale):
                                   (options.sharpness>0?PostprocessVariant::Sharpen:PostprocessVariant::Bypass);
    return plan;
}
} // namespace surfxr
