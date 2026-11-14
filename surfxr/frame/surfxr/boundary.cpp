#include "wire.hpp"

namespace surfxr::transport {
Bytes frame_boundary(const TrackingFrame& frame){
    if(!frame.boundary_sync_enabled||!frame.boundary||frame.boundary->origin_epoch!=frame.origin_epoch||
       !valid_stage_boundary(*frame.boundary))return {};
    const auto& b=*frame.boundary;
    Writer body;body.integer(0x56444244,4);body.integer(1,4);body.integer(b.origin_epoch,8);
    body.real(b.width);body.real(b.depth);body.integer(b.quads.size(),4);
    for(const auto& quad:b.quads)for(float value:quad)body.real(value);
    Writer outer(true);outer.integer(11,4);outer.blob(body.bytes);return std::move(outer.bytes);
}
}
