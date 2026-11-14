#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace surfxr::xr_detail {
// A streaming viewer can receive a newer complete image after xrWaitFrame
// wakes it. Spend only the unused part of this application interval waiting
// before selecting video; never change the runtime's target display time.
class VrPresentationPacer {
public:
    void set_enabled(bool enabled) noexcept {enabled_=enabled;}
    bool enabled() const noexcept {return enabled_;}
    static std::int64_t pacing_period(std::int64_t reported,double refresh_hz) noexcept {
        if(reported<2'000'000||reported>50'000'000)return 0;
        if(!std::isfinite(refresh_hz)||refresh_hz<20||refresh_hz>500)return reported;
        // A throttled application period can span multiple panel refreshes.
        // It must not give our voluntary wait a larger budget after a miss.
        return std::min(reported,static_cast<std::int64_t>(1e9/refresh_hz));
    }
    std::uint64_t before_wait_deadline(std::uint64_t previous_return,std::uint64_t now,
            std::uint64_t previous_display,std::int64_t period,bool new_image=false) const noexcept {
        if(!previous_return||now<previous_return||period<2'000'000||period>50'000'000)return 0;
        const auto interval=static_cast<std::uint64_t>(period);
        const auto next_return=previous_return+interval;
        const auto allowed=deadline(next_return,std::max(now,next_return),
            previous_display+interval,period,new_image);
        // Return to the runtime before its next normal wake, leaving 0.5 ms
        // for sleep overshoot and the call itself. A late/missing source may
        // cause a repeat, but must never stretch this application interval.
        const auto target=std::min(allowed,next_return-500'000);
        return target>now?target:0;
    }
    std::uint64_t deadline(std::uint64_t wait_return,std::uint64_t now,
                           std::uint64_t display,std::int64_t period,bool new_image=false) const noexcept {
        if(!enabled_||new_image||!wait_return||now<wait_return||display<=now||
           period<2'000'000||period>50'000'000)return 0;
        const auto interval=static_cast<std::uint64_t>(period);
        const auto budget=static_cast<std::uint64_t>(cpu_guard_ns_+gpu_guard_ns_)+1'000'000;
        if(budget>=interval||display-now<=budget)return 0;
        // Leave at least a quarter interval for submission even with tiny
        // measured shaders; a renderer/configuration change needs headroom.
        const auto delay=std::min(interval-budget,interval*3/4);
        const auto target=std::min(wait_return+delay,display-budget);
        return target>now?target:0;
    }
    void observe_cpu(std::uint64_t duration_ns) noexcept {
        cpu_guard_ns_=guard(cpu_guard_ns_,double(duration_ns),500'000.);
    }
    void observe_gpu(double reconstruction_ms,double enhancement_ms) noexcept {
        if(!std::isfinite(reconstruction_ms)||reconstruction_ms<0||
           !std::isfinite(enhancement_ms)||enhancement_ms<0)return;
        gpu_guard_ns_=guard(gpu_guard_ns_,(reconstruction_ms+enhancement_ms)*1e6,500'000.);
    }
private:
    static double guard(double previous,double duration,double minimum) noexcept {
        // React immediately to expensive work, then recover gradually. A late
        // frame reduces the following wait instead of extending its deadline.
        return std::max({minimum,previous*.97,std::min(duration*1.25,100'000'000.)});
    }
    bool enabled_=true;
    double cpu_guard_ns_=1'000'000.,gpu_guard_ns_=2'000'000.;
};
} // namespace surfxr::xr_detail
