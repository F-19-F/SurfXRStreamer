#include "surfxr/system_input.hpp"
#include <cmath>

namespace surfxr::frame_input {
bool valid_skeleton(const Skeleton& value) noexcept {
    if(!value.sampled_ns)return true;
    for(const auto& bone:value.bones){
        float norm=0;
        for(float v:bone.position)if(!std::isfinite(v)||std::abs(v)>2)return false;
        for(float v:bone.orientation){if(!std::isfinite(v))return false;norm+=v*v;}
        if(norm<.5f||norm>1.5f)return false;
    }
    return true;
}
}
