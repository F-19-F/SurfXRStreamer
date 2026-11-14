#include <winsock2.h>
#include "FrameWorkerParent.h"
#include "FrameWorkerStats.h"
#include "FrameWorkerScheduling.h"
#include "platform/win32/VideoEncoderNVENC.h"
#include "platform/win32/EncoderThreadScheduling.h"
#include "surfxr_server/IDRScheduler.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

extern Settings g_settings;
namespace {
std::atomic<frame_worker::Shared*> childShared{nullptr};
frame_worker::Control childControl{}; // encoding thread only
uint64_t lastParams = 0;
bool captureCorrupted = false; // encoding thread only
thread_local SOCKET daemonSocket = INVALID_SOCKET;
thread_local void* daemonOwner=nullptr;
thread_local FfiFrameWorkerSend daemonSend=nullptr;
bool result(uint32_t kind, uint64_t timestamp, uint32_t bytes, bool idr, bool repeated,
    uint32_t level, const unsigned char* data, uint32_t size) {
    using namespace frame_worker;
    auto* shared = childShared.load(); if (!shared || size > PayloadLimit) return false;
    for (auto& entry : shared->results) {
        if (InterlockedCompareExchange(&entry.state, Writing, Free) != Free) continue;
        entry.kind = kind; entry.timestamp = timestamp; entry.bytes = bytes;
        entry.idr = idr; entry.repeated = repeated; entry.size = size; entry.level = level;
        entry.serial = uint64_t(InterlockedIncrement64(&shared->resultSerial));
        if (size) std::memcpy(entry.payload, data, size);
        InterlockedExchange(&entry.state, Ready); return true;
    }
    InterlockedIncrement64(&shared->resultDrops); return false;
}
void control(frame_worker::Shared& shared) {
    using namespace frame_worker;
    if (acquire(shared.controlLock)) { childControl = shared.control; release(shared.controlLock); }
    const auto now = now_us();
    for (uint32_t hand = 0; hand < 2; ++hand) {
        const auto& h = childControl.hands[hand];
        FrameWorkerImportHaptics(hand, h.serial, h.amplitude,
            h.deadlineUs > now ? h.deadlineUs - now : 0, h.pendingSerial, h.pendingAmplitude,
            now >= h.pendingAtUs ? now - h.pendingAtUs : UINT64_MAX);
    }
}
int encode(frame_worker::Shared& shared, HANDLE wake) {
    using namespace frame_worker;
    g_settings = shared.settings;
    lastParams = 0; captureCorrupted = false;
    auto render = std::make_shared<CD3DRender>();
    if (!render->Initialize(g_settings.m_nAdapterIndex)) throw std::runtime_error("Frame worker D3D device unavailable");
    inherit_gpu_scheduling(shared, render->GetDevice());
    Mailbox mailbox(shared, render->GetDevice());
    VideoEncoderNVENC encoder(render, shared.width, shared.height); encoder.Initialize();
    const EncoderThreadScheduling scheduling;
    childControl = shared.control;
    InterlockedExchange(&shared.ready, 1);
    while (!load(shared.networkReady) && !load(shared.stop)) WaitForSingleObject(wake, 10);
    if (load(shared.stop)) { encoder.Shutdown(); render->Shutdown(); return 0; }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("Worker Winsock initialization failed");
    if(daemonSend){
        if(!FrameWorkerNetworkReadyDirect(daemonOwner,daemonSend,&shared.transport))
            throw std::runtime_error("Worker direct writer unavailable");
    }else{
    auto socket = daemonSocket;daemonSocket = INVALID_SOCKET;
    if(socket == INVALID_SOCKET)socket = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
        reinterpret_cast<WSAPROTOCOL_INFOW*>(shared.socketProtocol), 0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == INVALID_SOCKET) throw std::runtime_error("Worker socket handoff failed");
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0)) {
        closesocket(socket);
        throw std::runtime_error("Worker socket inheritance protection failed");
    }
    if (!FrameWorkerNetworkReady(uint64_t(socket), &shared.transport))
        throw std::runtime_error("Worker UDP writer unavailable");
    }
    InterlockedExchange(&shared.ready, 2);
    std::optional<size_t> held;
    uint64_t serial = 0, lastNew = 0, lastEncode = 0;
    IDRScheduler idr; idr.OnStreamStart(); Stats stats; SourceWait sourceWait;
    auto finish = [&] {
        if (held) { mailbox.finish(*held); held.reset(); }
        encoder.Shutdown(); FrameWorkerNetworkStop(); render->Shutdown(); WSACleanup();
    };
    try {
        while (!load(shared.stop)) {
            control(shared); stats.report(shared);
            if (InterlockedExchange(&shared.idrRequests, 0)) idr.InsertIDR();
            const auto newest = mailbox.newest(serial); const auto now = now_us();
            if (newest) {
                if (held) mailbox.finish(*held);
                held = newest; serial = shared.slots[*held].serial; lastNew = now;
                stats.source(shared.slots[*held]);
            }
            // Keep the exact source timestamp, views and crops during a stall.
            // Repeated pictures are not new poses and do not refresh freshness.
            const auto period = uint64_t(1'000'000 / std::clamp(childControl.framerate, 1.f, 240.f));
            const bool repeated = !newest;
            if (!held || (repeated && (now - lastNew < std::max<uint64_t>(20'000, 2 * period)
                                      || now - lastEncode < period))) {
                sourceWait.wait(wake); continue;
            }
            if (load(shared.stop)) break;
            const auto& source = shared.slots[*held];
            FrameWorkerBeginVideo(&source.metadata, source.timestamp, repeated);
            const auto started = now_us();
            encoder.Transmit(mailbox.texture(*held), source.presentation, source.timestamp, idr.CheckIDRInsertion());
            lastEncode = started; stats.encoded(started, repeated, period);
        }
    } catch (...) { finish(); throw; }
    finish(); return 0;
}
}
extern "C" int FrameWorkerRun(unsigned long long mapping, unsigned long long wake) {
    using namespace frame_worker;
    Handle mappingHandle(reinterpret_cast<HANDLE>(mapping)), wakeHandle(reinterpret_cast<HANDLE>(wake));
    auto* shared = static_cast<Shared*>(MapViewOfFile(mappingHandle.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) return 2;
    if (shared->magic != Magic || shared->version != Version || shared->bytes != sizeof(Shared)
        || !shared->width || !shared->height || shared->fixture) { UnmapViewOfFile(shared); return 3; }
    childShared.store(shared); int status = 0;
    try { status = encode(*shared, wakeHandle.get()); }
    catch (const std::exception& error) {
        FrameWorkerRelayLog(error.what(), 0); InterlockedExchange(&shared->failed, 1); status = 1;
        FrameWorkerNetworkStop();
    } catch (...) { InterlockedExchange(&shared->failed, 1); FrameWorkerNetworkStop(); status = 1; }
    childShared.store(nullptr); InterlockedExchange(&shared->ready,3); UnmapViewOfFile(shared); return status;
}
extern "C" int FrameWorkerDaemonRun(unsigned long long mapping,unsigned long long wake,unsigned long long socket) {
    daemonSocket=SOCKET(socket);
    const auto status=FrameWorkerRun(mapping,wake);
    if(daemonSocket!=INVALID_SOCKET){closesocket(daemonSocket);daemonSocket=INVALID_SOCKET;}
    return status;
}
extern "C" int FrameWorkerDaemonRunDirect(unsigned long long mapping,unsigned long long wake,void* owner,FfiFrameWorkerSend send) {
    daemonOwner=owner;daemonSend=send;
    const auto status=FrameWorkerRun(mapping,wake);
    daemonOwner=nullptr;daemonSend=nullptr;return status;
}
extern "C" void FrameWorkerRelayLog(const char* message, unsigned int level) {
    if (!message) return;
    const auto size = uint32_t(std::min<size_t>(std::strlen(message), 2048));
    result(2, 0, 0, false, false, level, reinterpret_cast<const unsigned char*>(message), size);
}
extern "C" void FrameWorkerRelayVideo(unsigned long long timestamp, unsigned int bytes,
    bool idr, bool repeated, const unsigned char* payload) {
    // Optional mirror/recording payloads are bounded and cannot block video.
    auto* shared = childShared.load(); if (!shared) return;
    const auto size = childControl.capture && (!captureCorrupted || idr) ? bytes : 0;
    if (result(1, timestamp, bytes, idr, repeated, 0, payload, size)) {
        if (size && idr) captureCorrupted = false;
    } else if (size) {
        captureCorrupted = true;
        InterlockedIncrement(&shared->idrRequests); // optional capture resumes at an IDR
    }
}
extern "C" void FrameWorkerRelayConfig(const unsigned char* data, unsigned int size, int codec) {
    using namespace frame_worker;
    auto* shared = childShared.load(); if (!shared) return;
    if (size > ConfigLimit) { InterlockedExchange(&shared->failed, 1); return; }
    if (!acquire(shared->configLock)) { FrameWorkerChildRequestIDR(); return; }
    if (size) std::memcpy(shared->config, data, size);
    shared->configSize = size; shared->configCodec = codec;
    InterlockedIncrement64(&shared->configGeneration); release(shared->configLock);
}
extern "C" FfiDynamicEncoderParams FrameWorkerEncoderParams() {
    const auto changed = lastParams != childControl.generation;
    lastParams = childControl.generation;
    return {unsigned(changed), childControl.bitrate, childControl.framerate};
}
extern "C" void FrameWorkerChildRequestIDR() {
    if (auto* shared = childShared.load()) InterlockedIncrement(&shared->idrRequests);
}
