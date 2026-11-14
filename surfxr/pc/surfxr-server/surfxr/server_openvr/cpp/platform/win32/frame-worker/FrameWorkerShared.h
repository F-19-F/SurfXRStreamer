#pragma once
#include "surfxr_server/bindings.h"
#include <windows.h>
#include <cstdint>

namespace frame_worker {
constexpr uint32_t Magic = 0x46575032, Version = 6, SlotCount = 3;
constexpr uint32_t ResultCount = 4, PayloadLimit = 8 * 1024 * 1024, ConfigLimit = 4096;
enum State : LONG { Free, Writing, Ready, Reading };
struct SourceSlot {
    alignas(8) volatile LONG state;
    uint64_t serial, presentation, timestamp, queuedUs, textureHandle;
    FfiFrameWorkerSource metadata;
};
struct HapticHand {
    uint64_t serial, deadlineUs, pendingSerial, pendingAtUs;
    float amplitude, pendingAmplitude;
};
struct Control {
    uint64_t generation, bitrate;
    float framerate;
    uint32_t capture;
    HapticHand hands[2];
};
struct ResultSlot {
    alignas(8) volatile LONG state;
    uint32_t kind, size, bytes, idr, repeated, level;
    uint64_t serial, timestamp;
    unsigned char payload[PayloadLimit];
};
struct Shared {
    uint32_t magic, version, bytes, width, height, fixture;
    uint32_t cpuClass;
    int32_t gpuClass, gpuThreadPriority;
    volatile LONG gpuSchedulingApplied;
    alignas(8) volatile LONG stop, ready, networkReady, failed, controlLock, configLock, idrRequests;
    volatile LONG64 attempts, staged, rejected, discarded, captureMaxUs, lastCaptureUs;
    volatile LONG64 resultSerial, resultDrops, configGeneration;
    Settings settings;
    SourceSlot slots[SlotCount];
    Control control;
    FfiFrameWorkerTransport transport;
    // WSAPROTOCOL_INFOW is copied once, before networkReady is published.
    alignas(8) unsigned char socketProtocol[1024];
    uint32_t configSize, configCodec;
    unsigned char config[ConfigLimit];
    ResultSlot results[ResultCount];
};
inline uint64_t now_us() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
    return uint64_t(ticks.QuadPart / frequency.QuadPart) * 1'000'000
        + uint64_t(ticks.QuadPart % frequency.QuadPart) * 1'000'000 / frequency.QuadPart;
}
inline LONG load(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
inline bool acquire(volatile LONG& value) { return InterlockedCompareExchange(&value, 1, 0) == 0; }
inline void release(volatile LONG& value) { InterlockedExchange(&value, 0); }
inline void maximum(volatile LONG64& target, uint64_t value) {
    auto old = InterlockedCompareExchange64(&target, 0, 0);
    while (old < LONG64(value)) {
        const auto observed = InterlockedCompareExchange64(&target, LONG64(value), old);
        if (observed == old) break;
        old = observed;
    }
}
}
