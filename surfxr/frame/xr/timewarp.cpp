#include "display_private.hpp"
#include "late_timewarp.hpp"

namespace surfxr {
void XrDisplay::Impl::late_timewarp(XrTime time,std::array<XrCompositionLayerProjectionView,2>& views){
    xr_detail::late_projection_timewarp(session_,stage_space_,time,xrLocateViews,views,vr_timewarp_);
}
} // namespace surfxr
