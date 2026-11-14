#include "EncoderFrameMailbox.h"
#include <stdexcept>

namespace {
void require(HRESULT result) {
    if (result != S_OK) throw std::runtime_error("Encoder GPU handoff failed");
}
}

EncoderFrameMailbox::EncoderFrameMailbox(ID3D11Device* captureDevice,
    ID3D11Device* encoderDevice, ID3D11Texture2D* source) {
    captureDevice->GetImmediateContext(&m_captureContext);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (auto& slot : m_slots) {
        require(captureDevice->CreateTexture2D(&desc, nullptr, &slot.captureTexture));
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        require(slot.captureTexture.As(&resource));
        HANDLE handle = nullptr; // legacy DXGI handle belongs to the resource
        require(resource->GetSharedHandle(&handle));
        require(encoderDevice->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(slot.encoderTexture.GetAddressOf())));
        require(slot.captureTexture.As(&slot.captureMutex));
        require(slot.encoderTexture.As(&slot.encoderMutex));
    }
}

std::optional<size_t> EncoderFrameMailbox::TryBeginCapture() {
    for (size_t i = 0; i < SlotCount; ++i) {
        auto& slot = m_slots[i];
        auto expected = State::Free;
        if (!slot.state.compare_exchange_strong(expected, State::Writing)) continue;
        // Never hold SteamVR's Present waiting for the encoder or its GPU work.
        if (slot.captureMutex->AcquireSync(0, 0) == S_OK) return i;
        slot.state.store(State::Free);
    }
    return std::nullopt;
}

void EncoderFrameMailbox::CancelCapture(size_t i) {
    require(m_slots[i].captureMutex->ReleaseSync(0));
    m_slots[i].state.store(State::Free);
}

void EncoderFrameMailbox::EndCapture(size_t i, ID3D11Texture2D* source,
    uint64_t presentationTime, uint64_t targetTimestampNs) {
    auto& slot = m_slots[i];
    m_captureContext->CopyResource(slot.captureTexture.Get(), source);
    m_captureContext->Flush();
    require(slot.captureMutex->ReleaseSync(1));
    slot.serial = ++m_nextSerial;
    slot.presentationTime = presentationTime;
    slot.targetTimestampNs = targetTimestampNs;
    slot.queuedAt = std::chrono::steady_clock::now();
    slot.state.store(State::Ready, std::memory_order_release);
}

void EncoderFrameMailbox::DiscardOlder(uint64_t serial) {
    for (auto& slot : m_slots) {
        if (slot.state.load(std::memory_order_acquire) != State::Ready
            || slot.serial >= serial) continue;
        // Only this consumer changes Ready to Reading. The producer cannot
        // rewrite it until the queued GPU copy has completed and key 0 returns.
        slot.state.store(State::Reading);
        if (slot.encoderMutex->AcquireSync(1, 0) != S_OK) {
            slot.state.store(State::Ready);
            continue;
        }
        require(slot.encoderMutex->ReleaseSync(0));
        slot.state.store(State::Free, std::memory_order_release);
        ++m_discarded;
    }
}

bool EncoderFrameMailbox::TryBeginEncode(Frame& frame) {
    std::optional<size_t> newest;
    for (size_t i = 0; i < SlotCount; ++i) {
        if (m_slots[i].state.load(std::memory_order_acquire) == State::Ready
            && (!newest || m_slots[i].serial > m_slots[*newest].serial)) newest = i;
    }
    if (!newest) return false;
    auto& slot = m_slots[*newest];
    slot.state.store(State::Reading);
    // Polling on the encoder thread stays bounded, including during shutdown.
    if (slot.encoderMutex->AcquireSync(1, 0) != S_OK) {
        slot.state.store(State::Ready);
        return false;
    }
    frame = {*newest, slot.encoderTexture.Get(), slot.presentationTime,
             slot.targetTimestampNs, slot.queuedAt};
    DiscardOlder(slot.serial);
    return true;
}

void EncoderFrameMailbox::EndEncode(size_t i) {
    require(m_slots[i].encoderMutex->ReleaseSync(0));
    m_slots[i].state.store(State::Free, std::memory_order_release);
}
