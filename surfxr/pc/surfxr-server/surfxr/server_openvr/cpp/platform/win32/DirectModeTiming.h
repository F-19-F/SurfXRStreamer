#pragma once
#include "surfxr_server/Logger.h"
#include <atomic>
#include <chrono>
#include <cstdint>

// DirectMode stages only: no texture, pose, process identity or packet data.
// Phase counters may overlap; their maxima must not be added together.
class DirectModeTiming {
public:
    static uint64_t NowUs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void Maximum(std::atomic<uint64_t>& target, uint64_t value) {
        auto previous = target.load(std::memory_order_relaxed);
        while (previous < value && !target.compare_exchange_weak(previous, value)) {}
    }
    class Phase {
    public:
        explicit Phase(std::atomic<uint64_t>& target) : target(target), started(NowUs()) {}
        ~Phase() { Maximum(target, NowUs() - started); }
        Phase(const Phase&) = delete;
        Phase& operator=(const Phase&) = delete;
    private:
        std::atomic<uint64_t>& target;
        uint64_t started;
    };
    void Present(unsigned layers) {
        const auto now = NowUs(), previous = lastPresent.exchange(now);
        if (previous) Maximum(presentGap, now - previous);
        ++presents;
        if (!layers) ++emptyLayers;
    }
    void Report() {
        const auto now = NowUs();
        if (now - window < 5'000'000) return;
        Info("Frame present timing: {\"seconds\":%.6f,\"presents\":%llu,"
             "\"empty_layers\":%llu,\"sync_failures\":%llu,\"capture_failures\":%llu,"
             "\"present_gap_max_us\":%llu,\"present_max_us\":%llu,"
             "\"submit_layer_max_us\":%llu,\"mutex_wait_max_us\":%llu,"
             "\"sync_wait_max_us\":%llu,\"copy_max_us\":%llu,\"stats_max_us\":%llu}",
             (now - window) / 1e6, presents.exchange(0), emptyLayers.exchange(0),
             syncFailures.exchange(0), captureFailures.exchange(0), presentGap.exchange(0),
             presentMax.exchange(0), submitMax.exchange(0), mutexMax.exchange(0),
             syncMax.exchange(0), copyMax.exchange(0), statsMax.exchange(0));
        window = now; // PostPresent's owning compositor thread only.
    }
    std::atomic<uint64_t> presentMax{0}, submitMax{0}, mutexMax{0}, syncMax{0}, copyMax{0}, statsMax{0};
    std::atomic<uint64_t> syncFailures{0}, captureFailures{0};
private:
    std::atomic<uint64_t> presents{0}, emptyLayers{0}, presentGap{0}, lastPresent{0};
    uint64_t window = NowUs();
};
