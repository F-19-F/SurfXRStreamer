#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <dxgi.h>
#include <optional>
#include <wrl/client.h>

// Three immutable GPU slots: capture owns Writing, the encoder owns Reading.
// Only unencoded Ready images can be discarded. Keyed mutexes protect GPU work
// across the two devices; state publication protects the exact frame metadata.
class EncoderFrameMailbox {
public:
    static constexpr size_t SlotCount = 3;
    struct Frame {
        size_t slot;
        ID3D11Texture2D* texture;
        uint64_t presentationTime;
        uint64_t targetTimestampNs;
        std::chrono::steady_clock::time_point queuedAt;
    };

    EncoderFrameMailbox(ID3D11Device* captureDevice, ID3D11Device* encoderDevice,
                        ID3D11Texture2D* source);
    std::optional<size_t> TryBeginCapture();
    void CancelCapture(size_t slot);
    void EndCapture(size_t slot, ID3D11Texture2D* source,
                    uint64_t presentationTime, uint64_t targetTimestampNs);
    bool TryBeginEncode(Frame& frame);
    void EndEncode(size_t slot);
    uint64_t TakeDiscarded() { return m_discarded.exchange(0); }

private:
    enum class State { Free, Writing, Ready, Reading };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> captureTexture, encoderTexture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> captureMutex, encoderMutex;
        std::atomic<State> state{State::Free};
        uint64_t serial = 0, presentationTime = 0, targetTimestampNs = 0;
        std::chrono::steady_clock::time_point queuedAt;
    };
    void DiscardOlder(uint64_t serial);
    std::array<Slot, SlotCount> m_slots;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_captureContext;
    uint64_t m_nextSerial = 0; // capture thread only
    std::atomic<uint64_t> m_discarded{0};
};
