#include "display_private.hpp"

namespace surfxr {
std::uint64_t XrDisplay::Impl::wait_vr_presentation(std::uint64_t wait_return,
        std::uint64_t display,std::int64_t period,const VrStereoFrame& initial,
        const std::atomic_bool& stop,bool next_interval){
    const auto started=clock_ns(Clock::now());
    // Never wait past a complete image that has not been submitted yet. At
    // equal source/display rates that can skip this image, then repeat the
    // following one. Only a would-be repeat should wait for another arrival.
    const bool new_image=!have_vr_timestamp_||initial.metadata->timestamp!=stats_.vr_last_timestamp;
    const auto deadline=next_interval?
        vr_pacer_.before_wait_deadline(wait_return,started,display,period,new_image):
        vr_pacer_.deadline(wait_return,started,display,period,new_image);
    if(!deadline)return 0;
    // No frame/slot mutex is held while waiting. Bound cancellation latency
    // without busy polling; decoding and tracking uploads keep their workers.
    auto now=started;
    while(now<deadline&&!stop.load(std::memory_order_relaxed)){
        const auto candidate=callbacks_.vr_frame?callbacks_.vr_frame():nullptr;
        // Leave final validity/mode/epoch handling to the authoritative late
        // snapshot. Stop at the first changed complete image, not a fixed
        // phase that can skip one image and repeat another at equal rates.
        if(!candidate||!valid_vr_pair(*candidate)||candidate->origin_epoch!=initial.origin_epoch||
           candidate->metadata->timestamp!=initial.metadata->timestamp)break;
        std::this_thread::sleep_until(Clock::time_point(std::chrono::nanoseconds(
            std::min(deadline,now+500'000))));
        now=clock_ns(Clock::now());
    }
    return now-started;
}
} // namespace surfxr
