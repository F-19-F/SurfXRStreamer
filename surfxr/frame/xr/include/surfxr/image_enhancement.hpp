#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace surfxr {
struct ImageEnhancementOptions {
    bool super_resolution=false;
    float sharpness=0; // Independent local sharpening, 0..1; zero disables it.
};
struct ImageEnhancementExtent {unsigned width=0,height=0;double scale=1;bool upscale=false;};
// E is the complete, already reconstructed eye, never the encoded foveated D.
inline ImageEnhancementExtent image_enhancement_extent(unsigned ew,unsigned eh,bool sr,unsigned target_width=2736){
    if(!ew||!eh||!target_width)throw std::invalid_argument("Invalid complete-eye extent");
    if(!sr||ew>=target_width)return {ew,eh,1,false};
    const std::uint64_t rounded=(std::uint64_t{eh}*target_width+ew/2)/ew;
    if(!rounded||rounded>std::numeric_limits<unsigned>::max())throw std::invalid_argument("Enhanced eye extent overflow");
    return {target_width,static_cast<unsigned>(rounded),double(target_width)/ew,true};
}
inline bool valid_image_enhancement_options(ImageEnhancementOptions options)noexcept{
    return std::isfinite(options.sharpness)&&options.sharpness>=0&&options.sharpness<=1;
}
} // namespace surfxr
