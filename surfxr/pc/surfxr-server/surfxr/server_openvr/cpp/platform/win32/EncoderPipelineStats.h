#pragma once

#include "surfxr_server/Logger.h"
#include <atomic>
#include <chrono>

// Numeric five-second windows, including silence. No textures or poses logged.
class EncoderPipelineStats {
public:
    using Clock = std::chrono::steady_clock;
    static uint64_t NowUs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    }
    void CaptureAttempt() {
        const auto now = NowUs();
        const auto previous = lastCapture.exchange(now);
        if (previous) Maximum(captureGap, now - previous);
        ++attempts;
    }
    void Captured(uint64_t started) { ++staged; Maximum(captureTime, NowUs() - started); }
    void Rejected() { ++rejected; }
    void Encoded(uint64_t started, uint64_t queueUs) {
        ++encoded;
        Maximum(encodeTime, NowUs() - started);
        Maximum(queueTime, queueUs);
    }
    void Report(bool independent, uint64_t discarded) {
        discardedPending += discarded;
        const auto now = NowUs();
        if (now - windowStart < 5'000'000) return;
        const auto last = lastCapture.load();
        Info("Frame encoder timing: {\"seconds\":%.6f,\"independent_device\":%u,"
             "\"capture_attempts\":%llu,\"staged\":%llu,\"stage_rejected\":%llu,"
             "\"encoded\":%llu,\"unencoded_discarded\":%llu,\"capture_max_us\":%llu,"
             "\"encode_max_us\":%llu,\"queue_max_us\":%llu,\"source_gap_max_us\":%llu,"
             "\"source_idle_us\":%llu}",
             (now - windowStart) / 1e6, unsigned(independent), attempts.exchange(0),
             staged.exchange(0), rejected.exchange(0), encoded.exchange(0), discardedPending,
             captureTime.exchange(0), encodeTime.exchange(0), queueTime.exchange(0),
             captureGap.exchange(0), last ? now - last : now - windowStart);
        windowStart = now;
        discardedPending = 0;
    }
private:
    static void Maximum(std::atomic<uint64_t>& target, uint64_t value) {
        auto old = target.load();
        while (old < value && !target.compare_exchange_weak(old, value)) {}
    }
    std::atomic<uint64_t> lastCapture{0}, attempts{0}, staged{0}, rejected{0}, encoded{0};
    std::atomic<uint64_t> captureGap{0}, captureTime{0}, encodeTime{0}, queueTime{0};
    uint64_t windowStart = NowUs(); // encoder thread only
    uint64_t discardedPending = 0;
};
