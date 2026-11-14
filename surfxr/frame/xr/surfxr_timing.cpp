#include "display_private.hpp"
namespace surfxr {
void XrDisplay::Impl::publish_surfxr_performance() noexcept {
    const auto a=surfxr_performance_.snapshot(clock_ns(Clock::now()));
    stats_.surfxr_timing_available=a.available;stats_.surfxr_total_ms=a.total_ms;stats_.surfxr_upstream_ms=a.upstream_ms;
    stats_.surfxr_decode_ms=a.decode_ms;stats_.surfxr_after_decode_ms=a.after_decode_ms;
}
} // namespace surfxr
