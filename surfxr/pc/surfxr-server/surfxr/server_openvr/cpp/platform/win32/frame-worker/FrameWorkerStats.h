#pragma once
#include "FrameWorkerShared.h"
#include "surfxr_server/Logger.h"
#include <algorithm>

namespace frame_worker {
class Stats {
public:
    void source(const SourceSlot& slot) {
        ++sources;
        sourceNonadvancing += lastTimestamp && slot.timestamp <= lastTimestamp;
        lastTimestamp = std::max(lastTimestamp, slot.timestamp);
        imageNonadvancing += lastImageTimestamp && slot.metadata.captureTimestampNs <= lastImageTimestamp;
        lastImageTimestamp = std::max(lastImageTimestamp, slot.metadata.captureTimestampNs);
        if (lastSource && slot.queuedUs >= lastSource) sourceGap = std::max(sourceGap, slot.queuedUs - lastSource);
        lastSource = slot.queuedUs;
        queueMax = std::max(queueMax, now_us() - slot.queuedUs);
    }
    void encoded(uint64_t started, bool repeated, uint64_t budget) {
        const auto elapsed = now_us() - started;
        ++encodes; repeats += repeated;
        encodeMax = std::max(encodeMax, elapsed);
        encodeTotal += elapsed; encodeOverBudget += elapsed > budget; encodeBudget = budget;
    }
    void report(Shared& shared) {
        const auto now = now_us(); if (now - window < 5'000'000) return;
        const auto seconds = (now - window) / 1e6;
        const auto attempts = InterlockedExchange64(&shared.attempts, 0);
        const auto staged = InterlockedExchange64(&shared.staged, 0);
        const auto rejected = InterlockedExchange64(&shared.rejected, 0);
        const auto discarded = InterlockedExchange64(&shared.discarded, 0);
        const auto captureMax = InterlockedExchange64(&shared.captureMaxUs, 0);
        const auto lastCapture = uint64_t(InterlockedCompareExchange64(&shared.lastCaptureUs, 0, 0));
        const auto idle = lastCapture ? now - lastCapture : now - window;
        Info("Frame encoder timing: {\"seconds\":%.6f,\"independent_device\":1,"
             "\"capture_attempts\":%llu,\"staged\":%llu,\"stage_rejected\":%llu,"
             "\"encoded\":%llu,\"unencoded_discarded\":%llu,\"capture_max_us\":%llu,"
             "\"encode_max_us\":%llu,\"queue_max_us\":%llu,\"source_gap_max_us\":%llu,\"source_idle_us\":%llu,"
             "\"encode_total_us\":%llu,\"encode_over_budget\":%llu,\"encode_budget_us\":%llu,"
             "\"source_nonadvancing_timestamps\":%llu,\"image_nonadvancing_timestamps\":%llu}",
             seconds, uint64_t(attempts), uint64_t(staged), uint64_t(rejected), encodes,
             uint64_t(discarded), uint64_t(captureMax), encodeMax, queueMax, sourceGap, idle,
             encodeTotal, encodeOverBudget, encodeBudget, sourceNonadvancing, imageNonadvancing);
        Info("Frame process timing: {\"seconds\":%.6f,\"pid\":%lu,\"source_frames\":%llu,"
             "\"encoded_frames\":%llu,\"repeated_frames\":%llu,\"source_idle_us\":%llu,"
             "\"telemetry_drops\":%llu,\"failed\":%u,\"cpu_class\":%lu,"
             "\"gpu_class\":%u,\"gpu_scheduling_applied\":%u}", seconds, GetCurrentProcessId(),
             sources, encodes, repeats, idle,
             uint64_t(InterlockedCompareExchange64(&shared.resultDrops, 0, 0)), unsigned(load(shared.failed)),
             GetPriorityClass(GetCurrentProcess()), unsigned(std::max(0, shared.gpuClass)),
             unsigned(load(shared.gpuSchedulingApplied)));
        window = now; sources = encodes = repeats = encodeMax = queueMax = sourceGap = 0;
        encodeTotal = encodeOverBudget = 0;
        sourceNonadvancing = 0;
        imageNonadvancing = 0;
    }
private:
    uint64_t window = now_us(), lastSource = 0, sources = 0, encodes = 0, repeats = 0;
    uint64_t encodeMax = 0, queueMax = 0, sourceGap = 0;
    uint64_t encodeTotal = 0, encodeOverBudget = 0, encodeBudget = 0;
    uint64_t lastTimestamp = 0, sourceNonadvancing = 0;
    uint64_t lastImageTimestamp = 0, imageNonadvancing = 0;
};
}
