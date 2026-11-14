#pragma once
#include "surfxr/boundary.hpp"

namespace surfxr::xr_detail {
// Private same-machine seqpacket; no persisted room geometry or public wire change.
struct BoundarySnapshot {
    std::uint64_t token=0;
    float width=0,depth=0;
    std::uint32_t count=0,sample_count=0;
    std::array<std::array<float,12>,StageBoundary::max_quads> quads{};
};
static_assert(sizeof(BoundarySnapshot)==3096);
// Separate datagram size; the existing boundary subscription bytes stay intact.
struct IpdSnapshot {
    std::uint32_t magic=0x49504431,samples=0,events=0;
    float meters=0;
};
static_assert(sizeof(IpdSnapshot)==16);
struct RefreshRequest {
    std::uint32_t magic=0x52465131;
    float hz=0;
    std::uint64_t serial=0;
};
struct RefreshSnapshot {
    std::uint32_t magic=0x52465331;
    std::int32_t result=0; // OpenVR settings error; -1 unsupported, -2 unknown parent app.
    std::uint64_t serial=0;
    float requested=0,actual=0;
    std::uint32_t app_known=0,reserved=0;
};
static_assert(sizeof(RefreshRequest)==16&&sizeof(RefreshSnapshot)==32);
}
