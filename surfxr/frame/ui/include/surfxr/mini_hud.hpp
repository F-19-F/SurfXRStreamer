#pragma once
#include "surfxr/surfxr_performance.hpp"
#include "surfxr/performance_overlay.hpp"
#include "surfxr/vr_ui.hpp"

namespace surfxr {
// Viewer-local presentation of existing meters; this owns no timing collector.
struct MiniHud {
    static constexpr unsigned width=320,height=136;
    struct Layout {
        UiPose pose;
        float width_m=0,height_m=0;
        unsigned active_height=0,rows=0;
    };
    struct Contents {std::array<std::string,4> lines{};unsigned count=0;};
    static Layout layout(const InputPreferences&) noexcept;
    static Contents contents(const PerformanceOverlay&,std::uint64_t now,const InputPreferences&,
                             const SurfxrPerformance::Snapshot&);
    // Fixed texture extent; the composition layer crops to layout.active_height.
    static VideoFrame raster(const PerformanceOverlay&,std::uint64_t now,std::uint64_t generation,
                             const InputPreferences&,const SurfxrPerformance::Snapshot&);
    static VideoFrame marker(const InputPreferences&,std::uint64_t generation);
    // Both poses are in VIEW space. Never pass the clipped foveation/wire gaze.
    static bool gaze_hits(const Layout&,const UiPose& gaze) noexcept;
};

class MiniHudVisibility {
public:
    struct State {bool values=false,marker=false;};
    static constexpr std::uint64_t dwell_ns=80'000'000,grace_ns=250'000'000;
    static constexpr std::uint64_t freshness_ns=100'000'000;
    // Zero sample time means the runtime exposes no timestamp: rely on this
    // successful tracked query's freshness, without claiming sensor sample age.
    // Predicted gaze may legitimately carry a bounded future sample time.
    static bool fresh_sample(std::uint64_t sample_ns,std::uint64_t query_ns,std::uint64_t now_ns) noexcept;
    void reset() noexcept {*this={};}
    // valid means a focused, active, fresh eye-gaze sample with valid pose flags.
    State update(const InputPreferences&,const MiniHud::Layout&,const UiPose& gaze,
                 bool valid,std::uint64_t now) noexcept;
private:
    std::uint64_t dwell_at_=0,last_hit_=0,last_update_=0;
    bool dwell_pending_=false,visible_=false,has_update_=false;
};
} // namespace surfxr
