#pragma once
#include <windows.h>
#include <avrt.h>
#include "surfxr_server/Logger.h"
#pragma comment(lib, "avrt.lib")

// Register/revert on this worker; leave process priority and affinity alone.
class EncoderThreadScheduling {
public:
    EncoderThreadScheduling() {
        DWORD index = 0;
        m_handle = AvSetMmThreadCharacteristicsW(L"Capture", &index);
        const auto registeredError = m_handle ? 0 : GetLastError();
        const bool boosted = m_handle && AvSetMmThreadPriority(m_handle, AVRT_PRIORITY_HIGH);
        Info("Frame encoder scheduling: registered=%u boosted=%u error=%lu",
             unsigned(m_handle != nullptr), unsigned(boosted),
             registeredError ? registeredError : (boosted ? 0 : GetLastError()));
        if (!m_handle) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    ~EncoderThreadScheduling() { if (m_handle) AvRevertMmThreadCharacteristics(m_handle); }
    EncoderThreadScheduling(const EncoderThreadScheduling&) = delete;
    EncoderThreadScheduling& operator=(const EncoderThreadScheduling&) = delete;
private:
    HANDLE m_handle = nullptr;
};
