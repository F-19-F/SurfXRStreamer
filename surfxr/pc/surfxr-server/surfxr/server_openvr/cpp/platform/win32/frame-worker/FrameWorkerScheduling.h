#pragma once
#include "FrameWorkerShared.h"
#include "surfxr_server/Logger.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>

namespace frame_worker {
// Private high-resolution polling: the child must not depend on SteamVR's
// timer resolution while no capture event is being signaled.
class SourceWait {
public:
    SourceWait() : m_timer(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
    ~SourceWait() { if (m_timer) CloseHandle(m_timer); }
    SourceWait(const SourceWait&) = delete;
    SourceWait& operator=(const SourceWait&) = delete;
    void wait(HANDLE wake) const {
        LARGE_INTEGER due{}; due.QuadPart = -10'000; // one millisecond, 100 ns units
        if (m_timer && SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[]{wake, m_timer};
            WaitForMultipleObjects(2, handles, FALSE, 50);
        } else WaitForSingleObject(wake, 1);
    }
private:
    HANDLE m_timer = nullptr;
};
// A new process defaults to Normal even when vrserver is High/Realtime GPU.
// Copy the actual parent's levels; do not choose a higher scheduling policy.
inline void snapshot_scheduling(Shared& shared, ID3D11Device* device) {
    shared.cpuClass = GetPriorityClass(GetCurrentProcess());
    if (!shared.cpuClass) shared.cpuClass = NORMAL_PRIORITY_CLASS;
    shared.gpuClass = -1; shared.gpuThreadPriority = 0;
    const auto module = GetModuleHandleW(L"gdi32.dll");
    using Query = LONG(WINAPI*)(HANDLE, int*);
    const auto query = reinterpret_cast<Query>(module ? GetProcAddress(module,
        "D3DKMTGetProcessSchedulingPriorityClass") : nullptr);
    int priority = 0;
    if (query && query(GetCurrentProcess(), &priority) == 0 && priority >= 0 && priority <= 5)
        shared.gpuClass = priority;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        INT relative = 0;
        if (SUCCEEDED(dxgi->GetGPUThreadPriority(&relative)) && relative >= -7 && relative <= 7)
            shared.gpuThreadPriority = relative;
    }
}
inline void inherit_gpu_scheduling(Shared& shared, ID3D11Device* device) {
    const auto module = GetModuleHandleW(L"gdi32.dll");
    using Set = LONG(WINAPI*)(HANDLE, int);
    const auto set = reinterpret_cast<Set>(module ? GetProcAddress(module,
        "D3DKMTSetProcessSchedulingPriorityClass") : nullptr);
    const auto status = set && shared.gpuClass >= 0
        ? set(GetCurrentProcess(), shared.gpuClass) : LONG(0xc0000002);
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    auto relative = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(relative)) relative = dxgi->SetGPUThreadPriority(shared.gpuThreadPriority);
    InterlockedExchange(&shared.gpuSchedulingApplied, status == 0 && SUCCEEDED(relative));
    Info("Frame worker scheduling inheritance: cpu=%lu gpu=%d relative=%d applied=%u status=0x%lx",
        GetPriorityClass(GetCurrentProcess()), shared.gpuClass, shared.gpuThreadPriority,
        unsigned(load(shared.gpuSchedulingApplied)), static_cast<unsigned long>(status));
}
}
