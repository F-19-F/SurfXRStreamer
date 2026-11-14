// Extend gaze_atlas_test with real child-process GPU leases and Job teardown.
#include "frame_worker_test.h"
#include "platform/win32/frame-worker/FrameWorkerParent.h"
#include "platform/win32/frame-worker/FrameWorkerScheduling.h"
#include <stdexcept>
#include <vector>
#include <thread>
#include <chrono>
using Microsoft::WRL::ComPtr;
using namespace frame_worker;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void wait(Shared& shared, LONG phase, HANDLE process) {
    const auto until = now_us() + 3'000'000;
    while (load(shared.ready) != phase) {
        require(!load(shared.failed) && now_us() < until, "process_fixture_timeout");
        require(WaitForSingleObject(process, 1) == WAIT_TIMEOUT, "process_fixture_exited");
    }
}
void pixels(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, unsigned value) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)), "process_readback");
    context->CopyResource(readback.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "process_readback_map");
    for (unsigned y = 0; y < desc.Height; ++y) for (unsigned x = 0; x < desc.Width * 4; ++x)
        require(static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x] == value,
            "process_immutable_pixels");
    context->Unmap(readback.Get(), 0);
}
}
int frame_worker_fixture(unsigned long long mapping, unsigned long long wake) {
    Handle mappingHandle(reinterpret_cast<HANDLE>(mapping)), wakeHandle(reinterpret_cast<HANDLE>(wake));
    auto* shared = static_cast<Shared*>(MapViewOfFile(mappingHandle.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) return 2;
    int result = 0;
    try {
        require(shared->magic == Magic && shared->fixture == 1, "fixture_private_mapping");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "process_warp_device");
        Mailbox mailbox(*shared, device.Get());
        require(GetPriorityClass(GetCurrentProcess()) == shared->cpuClass, "process_parent_cpu_scheduling_inherited");
        inherit_gpu_scheduling(*shared, device.Get());
        InterlockedExchange(&shared->ready, 1);
        std::optional<size_t> held; SourceWait sourceWait;
        while (!held && !load(shared->stop)) { held = mailbox.newest(0); WaitForSingleObject(wakeHandle.get(), 1); }
        if (held) {
            require(shared->slots[*held].timestamp == 101, "process_original_timestamp");
            require(shared->slots[*held].metadata.crops[1][0] == 17, "process_original_crop");
            pixels(device.Get(), context.Get(), mailbox.texture(*held), 21);
            InterlockedExchange(&shared->ready, 2);
            // The producer/observer can stop working while this process keeps
            // accessing the leased image. No lease is overwritten or released.
            while (!load(shared->networkReady) && !load(shared->stop)) {
                pixels(device.Get(), context.Get(), mailbox.texture(*held), 21);
                InterlockedIncrement64(&shared->resultSerial);
                sourceWait.wait(wakeHandle.get());
            }
            pixels(device.Get(), context.Get(), mailbox.texture(*held), 21);
            mailbox.finish(*held); held.reset();
            if (!load(shared->stop)) {
                while (!held) { held = mailbox.newest(1); WaitForSingleObject(wakeHandle.get(), 1); }
                require(shared->slots[*held].timestamp == 303 && shared->slots[*held].presentation == 1303,
                    "process_latest_exact_metadata");
                require(shared->slots[*held].metadata.crops[0][1] == 29, "process_latest_exact_crop");
                pixels(device.Get(), context.Get(), mailbox.texture(*held), 65);
                mailbox.finish(*held); held.reset();
                InterlockedExchange(&shared->ready, 3);
            }
        }
        while (!load(shared->stop)) WaitForSingleObject(wakeHandle.get(), 1);
        if (shared->control.capture == 99) for (;;) Sleep(1000); // bounded Job kill test
        if (held) mailbox.finish(*held);
    } catch (...) { InterlockedExchange(&shared->failed, 1); result = 1; }
    UnmapViewOfFile(shared); return result;
}
void check_process_handoff(ID3D11Device* device, ID3D11DeviceContext* context) {
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "fixture_executable");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32; desc.Height = 16; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source)), "process_source_texture");
    {
        Parent parent(device, source.Get(), true, executable);
        auto& shared = parent.shared(); BOOL inJob = FALSE;
        require(IsProcessInJob(parent.process(), nullptr, &inJob) && inJob, "child_attached_to_job_at_creation");
        auto submit = [&](unsigned value, uint64_t timestamp) {
            const auto slot = parent.begin_capture(); require(slot.has_value(), "process_free_capture_slot");
            const std::vector<unsigned char> data(32 * 16 * 4, static_cast<unsigned char>(value));
            context->UpdateSubresource(source.Get(), 0, nullptr, data.data(), 32 * 4, 0);
            FfiFrameWorkerSource metadata{}; metadata.valid = 1;
            metadata.crops[1][0] = 17; metadata.crops[0][1] = 29;
            parent.publish(*slot, source.Get(), timestamp + 1000, timestamp, metadata, now_us());
        };
        submit(21, 101); wait(shared, 2, parent.process());
        submit(43, 202); submit(65, 303);
        require(!parent.begin_capture().has_value(), "process_full_mailbox_does_not_wait");
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> query;
        require(SUCCEEDED(device->CreateQuery(&queryDesc, &query)), "process_copy_completion_query");
        context->End(query.Get()); context->Flush();
        const auto until = now_us() + 2'000'000;
        while (context->GetData(query.Get(), nullptr, 0, 0) != S_OK)
            require(now_us() < until, "process_copy_completion_timeout");
        const auto before = InterlockedCompareExchange64(&shared.resultSerial, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(InterlockedCompareExchange64(&shared.resultSerial, 0, 0) > before,
            "child_continues_when_producer_and_observer_pause");
        InterlockedExchange(&shared.networkReady, 1); wait(shared, 3, parent.process());
        require(shared.discarded == 1, "process_only_unencoded_old_image_discarded");
        const auto canceled = parent.begin_capture(); require(canceled.has_value(), "process_recycled_slot");
        parent.cancel(*canceled);
        parent.stop(); DWORD exit = 1;
        require(GetExitCodeProcess(parent.process(), &exit) && exit == 0, "process_cooperative_exit");
        parent.pump();
        require(!parent.begin_capture(), "retired_encoder_rejects_new_capture");
        InterlockedExchange(&shared.failed, 1);bool failed=false;
        try { parent.pump(); } catch (const std::runtime_error&) { failed=true; }
        require(failed,"retirement_does_not_hide_encoder_failure");
        InterlockedExchange(&shared.failed, 0);
    }
    {
        Parent stuck(device, source.Get(), true, executable);
        stuck.shared().control.capture = 99;
        const auto started = now_us(); stuck.stop(); DWORD exit = 0;
        require(GetExitCodeProcess(stuck.process(), &exit) && exit == 1, "process_private_job_forced_exit");
        require(now_us() - started < 4'500'000, "process_shutdown_is_bounded");
    }
}
// Parent fixture never negotiates a socket, changes settings or calls OpenVR.
extern "C" int FrameWorkerRun(unsigned long long, unsigned long long) { return 2; }
extern "C" FfiDynamicEncoderParams GetDynamicEncoderParams() { return {}; }
extern "C" bool FrameWorkerTransport(FfiFrameWorkerTransport*) { return false; }
extern "C" bool FrameWorkerCaptureEnabled() { return false; }
extern "C" bool FrameWorkerDaemonRequired() { return false; }
extern "C" void FrameWorkerObserve(unsigned long long, unsigned int, bool, bool, const unsigned char*, unsigned int) {}
extern "C" void SetVideoConfigNals(const unsigned char*, int, int) {}
extern "C" void LogError(const char*) {}
extern "C" void LogWarn(const char*) {}
extern "C" void LogInfo(const char*) {}
void Warn(const char*, ...) {}
