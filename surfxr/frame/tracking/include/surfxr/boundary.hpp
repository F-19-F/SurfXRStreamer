#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace surfxr {
// OpenVR live collision quads in standing/STAGE space, meters, xyz.
// Separate from the inscribed rectangular OpenXR play area.
struct StageBoundary {
    static constexpr unsigned max_quads=64;
    std::uint64_t origin_epoch=0;
    float width=0,depth=0;
    std::vector<std::array<float,12>> quads;
};
inline bool valid_stage_boundary(const StageBoundary& b){
    const auto finite=[](float v){return v==v&&v>=-100&&v<=100;};
    if(!finite(b.width)||!finite(b.depth)||b.width<=0||b.depth<=0||
       b.quads.size()<3||b.quads.size()>StageBoundary::max_quads)return false;
    for(const auto& quad:b.quads)for(float value:quad)if(!finite(value))return false;
    return true;
}
}
