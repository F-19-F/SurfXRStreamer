#pragma once
#include "FrameWorkerMailbox.h"
#include <memory>
#include <mutex>
#include <string>

namespace frame_worker {
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : m_value(value) {}
    ~Handle() { if (m_value) CloseHandle(m_value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE value) { if (m_value) CloseHandle(m_value); m_value = value; }
    HANDLE get() const { return m_value; }
private: HANDLE m_value = nullptr;
};
class Parent {
public:
    Parent(ID3D11Device* device, ID3D11Texture2D* source, bool fixture = false,
           const std::wstring& executable = {});
    ~Parent();
    std::optional<size_t> begin_capture() { return load(m_shared->stop) ? std::nullopt : m_mailbox->begin_capture(); }
    void cancel(size_t slot) { m_mailbox->cancel(slot); }
    void publish(size_t slot, ID3D11Texture2D* source, uint64_t presentation, uint64_t timestamp,
                 const FfiFrameWorkerSource& metadata, uint64_t startedUs);
    void request_idr() { InterlockedIncrement(&m_shared->idrRequests); }
    void haptics(uint32_t hand, float amplitude, uint64_t durationUs);
    void pump();
    void stop();
    Shared& shared() { return *m_shared; }
    HANDLE process() const { return m_process.get(); }
private:
    void launch(const std::wstring& executable);
    bool alive() const;
    Handle m_mapping, m_wake, m_job, m_process, m_daemon;
    DWORD m_workerPid = 0;
    Shared* m_shared = nullptr;
    std::unique_ptr<Mailbox> m_mailbox;
    std::mutex m_controlMutex;
    Control m_control{};
    uint64_t m_configGeneration = 0, m_lastControlUs = 0;
};
}
