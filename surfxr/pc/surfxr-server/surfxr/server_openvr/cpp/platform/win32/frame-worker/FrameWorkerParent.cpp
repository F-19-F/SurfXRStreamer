#include <winsock2.h>
#include "FrameWorkerParent.h"
#include "FrameWorkerScheduling.h"
#include "FrameDaemonIPC.h"
#include "surfxr_server/Logger.h"
#include "surfxr_server/Settings.h"
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#pragma comment(lib, "ws2_32.lib")

namespace {
std::mutex activeMutex;
frame_worker::Parent* active = nullptr;
void require(bool ok, const char* operation = "startup") {
    if (!ok) throw std::runtime_error(std::string("Frame worker ") + operation
        + " failed (Windows error " + std::to_string(GetLastError()) + ")");
}
struct Attributes {
    std::vector<unsigned char> memory;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    HANDLE job = nullptr; // attribute values must survive through CreateProcess
    Attributes(const std::vector<HANDLE>& handles, HANDLE jobHandle) : job(jobHandle) {
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        memory.resize(size); list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        require(InitializeProcThreadAttributeList(list, 2, 0, &size) != FALSE, "attribute initialization");
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(handles.data()), handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const auto error = GetLastError(); DeleteProcThreadAttributeList(list); list = nullptr;
            SetLastError(error); require(false, "handle inheritance");
        }
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                &job, sizeof(job), nullptr, nullptr)) {
            const auto error = GetLastError(); DeleteProcThreadAttributeList(list); list = nullptr;
            SetLastError(error); require(false, "job assignment");
        }
    }
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
}
namespace frame_worker {
Parent::Parent(ID3D11Device* device, ID3D11Texture2D* source, bool fixture,
    const std::wstring& executable) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    m_mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
        0, sizeof(Shared), nullptr)); require(m_mapping.get() != nullptr);
    m_shared = static_cast<Shared*>(MapViewOfFile(m_mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    require(m_shared != nullptr);
    try {
        m_shared->magic = Magic; m_shared->version = Version; m_shared->bytes = sizeof(Shared);
        m_shared->settings = *Settings_Instance(); m_shared->fixture = fixture;
        snapshot_scheduling(*m_shared, device);
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        m_shared->width = desc.Width; m_shared->height = desc.Height;
        m_mailbox = std::make_unique<Mailbox>(*m_shared, device, source);
        m_wake.reset(CreateEventW(&attributes, FALSE, FALSE, nullptr)); require(m_wake.get() != nullptr);
        m_control.framerate = float(m_shared->settings.m_refreshRate);
        m_control.bitrate = 30'000'000; m_control.generation = 1;
        if (!fixture) {
            const auto params = GetDynamicEncoderParams();
            if (params.updated) { m_control.bitrate = params.bitrate_bps; m_control.framerate = params.framerate; }
        }
        m_shared->control = m_control;
        launch(executable);
        const auto deadline = now_us() + 15'000'000;
        while (!load(m_shared->ready)) {
            require(!load(m_shared->failed) && alive(), "child GPU initialization");
            Sleep(1);
            require(now_us() < deadline, "child GPU deadline");
        }
        if (!fixture) {
            require(FrameWorkerTransport(&m_shared->transport));
            static_assert(sizeof(WSAPROTOCOL_INFOW) <= sizeof(m_shared->socketProtocol));
            if (!m_daemon.get() && WSADuplicateSocketW(SOCKET(m_shared->transport.socket),
                m_workerPid, reinterpret_cast<WSAPROTOCOL_INFOW*>(m_shared->socketProtocol)) != 0) {
                SetLastError(WSAGetLastError()); require(false, "video socket duplication");
            }
            InterlockedExchange(&m_shared->networkReady, 1); SetEvent(m_wake.get());
            while (load(m_shared->ready) != 2) {
                require(!load(m_shared->failed) && alive()); Sleep(1);
                require(now_us() < deadline);
            }
            std::lock_guard<std::mutex> guard(activeMutex); active = this;
            Info("Frame encoder owner ready: pid=%lu daemon=%u", m_workerPid, m_daemon.get()!=nullptr);
        }
    } catch (...) {
        if (m_shared) InterlockedExchange(&m_shared->stop, 1);
        if (m_wake.get()) SetEvent(m_wake.get());
        if (m_job.get()) TerminateJobObject(m_job.get(), 1);
        if (m_process.get()) WaitForSingleObject(m_process.get(), 1000);
        m_mailbox.reset(); UnmapViewOfFile(m_shared); m_shared = nullptr; throw;
    }
}
void Parent::launch(const std::wstring& executable) {
    if (!m_shared->fixture && FrameWorkerDaemonRequired()) {
        HANDLE pipe=CreateFileW(DaemonPipe,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE){
            m_daemon.reset(pipe);DaemonRequest request;
            request.mapping=reinterpret_cast<uint64_t>(m_mapping.get());request.wake=reinterpret_cast<uint64_t>(m_wake.get());
            DWORD bytes=0;DaemonReply reply{};
            require(WriteFile(pipe,&request,sizeof(request),&bytes,nullptr)&&bytes==sizeof(request),"daemon attachment");
            require(ReadFile(pipe,&reply,sizeof(reply),&bytes,nullptr)&&bytes==sizeof(reply)&&reply.magic==request.magic&&reply.pid,"daemon admission");
            m_workerPid=reply.pid;return;
        }
        require(!FrameWorkerDaemonRequired(),"required daemon unavailable");
    }
    auto path = std::filesystem::path(executable);
    if (path.empty()) {
        HMODULE module = nullptr;
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&FrameWorkerRun), &module) != FALSE);
        wchar_t file[32768]{}; require(GetModuleFileNameW(module, file, 32768) != 0);
        path = std::filesystem::path(file).parent_path() / L"surfxr_frame_worker.exe";
    }
    require(path.is_absolute());
    m_job.reset(CreateJobObjectW(nullptr, nullptr)); require(m_job.get() != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE);
    std::vector<HANDLE> inherited{m_mapping.get(), m_wake.get()};
    for (auto handle : m_mailbox->handles()) inherited.push_back(handle);
    Attributes attributes(inherited, m_job.get());
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + path.wstring() + L"\" "
        + (m_shared->fixture ? L"--frame-worker-fixture " : L"--frame-worker ")
        + std::to_wstring(reinterpret_cast<uint64_t>(m_mapping.get())) + L" "
        + std::to_wstring(reinterpret_cast<uint64_t>(m_wake.get()));
    require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | m_shared->cpuClass, nullptr, path.parent_path().c_str(),
        &startup.StartupInfo, &process) != FALSE, "process creation");
    m_process.reset(process.hProcess); CloseHandle(process.hThread);
    m_workerPid=GetProcessId(m_process.get());
}
bool Parent::alive() const {
    if(m_daemon.get())return PeekNamedPipe(m_daemon.get(),nullptr,0,nullptr,nullptr,nullptr)!=FALSE;
    return m_process.get()&&WaitForSingleObject(m_process.get(),0)==WAIT_TIMEOUT;
}
Parent::~Parent() {
    stop(); m_mailbox.reset();
    if (m_shared) UnmapViewOfFile(m_shared);
}
void Parent::stop() {
    { std::lock_guard<std::mutex> guard(activeMutex); if (active == this) active = nullptr; }
    if (!m_shared) return;
    InterlockedExchange(&m_shared->stop, 1); SetEvent(m_wake.get());
    if(m_daemon.get()){
        const auto deadline=now_us()+3'000'000;
        while(alive()&&!load(m_shared->failed)&&load(m_shared->ready)!=3&&now_us()<deadline)Sleep(1);
        m_daemon.reset(nullptr);return;
    }
    if (m_process.get() && WaitForSingleObject(m_process.get(), 3000) == WAIT_TIMEOUT) {
        Warn("Frame worker exceeded shutdown deadline; terminating its private job");
        TerminateJobObject(m_job.get(), 1); WaitForSingleObject(m_process.get(), 1000);
    }
}
void Parent::publish(size_t slot, ID3D11Texture2D* source, uint64_t presentation,
    uint64_t timestamp, const FfiFrameWorkerSource& metadata, uint64_t startedUs) {
    m_mailbox->publish(slot, source, presentation, timestamp, metadata);
    maximum(m_shared->captureMaxUs, now_us() - startedUs); SetEvent(m_wake.get());
}
void Parent::haptics(uint32_t hand, float amplitude, uint64_t durationUs) {
    if (hand > 1 || !std::isfinite(amplitude)) return;
    std::lock_guard<std::mutex> guard(m_controlMutex);
    auto& state = m_control.hands[hand]; const auto now = now_us();
    ++state.serial; state.amplitude = std::clamp(amplitude, 0.f, 1.f);
    state.deadlineUs = now + std::min<uint64_t>(durationUs, 5'000'000);
    if (state.amplitude > 0 && durationUs) {
        ++state.pendingSerial; state.pendingAmplitude = state.amplitude; state.pendingAtUs = now;
    }
}
void Parent::pump() {
    if (load(m_shared->failed))
        throw std::runtime_error("Frame encoder/send process exited; reconnect required");
    // The actual daemon encoder can retire before source-side DeinitializeStreaming.
    // A requested stop is normal even after its pipe closes; failures remain errors.
    if (load(m_shared->stop)) return;
    if (!alive())
        throw std::runtime_error("Frame encoder/send process exited; reconnect required");
    const auto now = now_us();
    if (!m_shared->fixture && now - m_lastControlUs >= 8'000) {
        const auto params = GetDynamicEncoderParams();
        const auto capture = FrameWorkerCaptureEnabled();
        std::lock_guard<std::mutex> guard(m_controlMutex);
        if (params.updated) { m_control.bitrate = params.bitrate_bps; m_control.framerate = params.framerate; ++m_control.generation; }
        m_control.capture = capture; m_lastControlUs = now;
    }
    { std::lock_guard<std::mutex> guard(m_controlMutex);
      if (acquire(m_shared->controlLock)) { m_shared->control = m_control; release(m_shared->controlLock); } }
    if (acquire(m_shared->configLock)) {
        const auto generation = uint64_t(InterlockedCompareExchange64(&m_shared->configGeneration, 0, 0));
        std::vector<unsigned char> config;
        const auto codec = m_shared->configCodec;
        if (generation != m_configGeneration && m_shared->configSize <= ConfigLimit) {
            config.assign(m_shared->config, m_shared->config + m_shared->configSize); m_configGeneration = generation;
        }
        release(m_shared->configLock);
        if (!config.empty()) SetVideoConfigNals(config.data(), int(config.size()), int(codec));
    }
    // Process results in publication order. Shared locks never span callbacks.
    for (;;) {
        ResultSlot* first = nullptr;
        for (auto& result : m_shared->results)
            if (load(result.state) == Ready && (!first || result.serial < first->serial)) first = &result;
        if (!first) break;
        InterlockedExchange(&first->state, Reading);
        const auto kind = first->kind, size = first->size, bytes = first->bytes, level = first->level;
        const auto timestamp = first->timestamp; const bool idr = first->idr != 0, repeated = first->repeated != 0;
        require(size <= PayloadLimit);
        std::vector<unsigned char> data(first->payload, first->payload + size);
        InterlockedExchange(&first->state, Free);
        if (kind == 1) FrameWorkerObserve(timestamp, bytes, idr, repeated, data.data(), size);
        else if (kind == 2) {
            data.push_back(0);
            if (level == 0) LogError(reinterpret_cast<char*>(data.data()));
            else if (level == 1) LogWarn(reinterpret_cast<char*>(data.data()));
            else LogInfo(reinterpret_cast<char*>(data.data()));
        }
    }
}
}
extern "C" void FrameWorkerUpdateHaptics(unsigned int hand, float amplitude, unsigned long long durationUs) {
    std::lock_guard<std::mutex> guard(activeMutex);
    if (active) active->haptics(hand, amplitude, durationUs);
}
