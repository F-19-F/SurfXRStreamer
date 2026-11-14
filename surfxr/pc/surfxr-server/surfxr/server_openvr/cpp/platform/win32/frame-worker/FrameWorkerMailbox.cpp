#include "FrameWorkerMailbox.h"
#include <stdexcept>
#include <algorithm>

namespace frame_worker {
namespace { void check(HRESULT hr) { if (hr != S_OK) throw std::runtime_error("Frame process GPU handoff failed"); } }
Mailbox::Mailbox(Shared& shared, ID3D11Device* device, ID3D11Texture2D* source)
    : m_shared(shared), m_producer(true) {
    device->GetImmediateContext(&m_context);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    try {
        for (size_t i = 0; i < SlotCount; ++i) {
            check(device->CreateTexture2D(&desc, nullptr, &m_textures[i]));
            Microsoft::WRL::ComPtr<IDXGIResource1> resource;
            check(m_textures[i].As(&resource));
            check(resource->CreateSharedHandle(&attributes,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &m_handles[i]));
            check(m_textures[i].As(&m_mutexes[i]));
            m_shared.slots[i].textureHandle = reinterpret_cast<uint64_t>(m_handles[i]);
        }
    } catch (...) { for (auto handle : m_handles) if (handle) CloseHandle(handle); throw; }
}
Mailbox::Mailbox(Shared& shared, ID3D11Device* device) : m_shared(shared), m_producer(false) {
    Microsoft::WRL::ComPtr<ID3D11Device1> device1; check(device->QueryInterface(IID_PPV_ARGS(&device1)));
    for (size_t i = 0; i < SlotCount; ++i) {
        const auto handle = reinterpret_cast<HANDLE>(m_shared.slots[i].textureHandle);
        const auto opened=device1->OpenSharedResource1(handle, IID_PPV_ARGS(&m_textures[i]));
        m_shared.slots[i].textureHandle=0;
        CloseHandle(handle); // The opened resource owns its GPU lifetime.
        check(opened);
        check(m_textures[i].As(&m_mutexes[i]));
    }
}
Mailbox::~Mailbox() { if (m_producer) for (auto handle : m_handles) if (handle) CloseHandle(handle); }
std::optional<size_t> Mailbox::begin_capture() {
    InterlockedIncrement64(&m_shared.attempts);
    InterlockedExchange64(&m_shared.lastCaptureUs, LONG64(now_us()));
    for (size_t i = 0; i < SlotCount; ++i) {
        if (InterlockedCompareExchange(&m_shared.slots[i].state, Writing, Free) != Free) continue;
        if (m_mutexes[i]->AcquireSync(0, 0) == S_OK) return i;
        InterlockedExchange(&m_shared.slots[i].state, Free);
    }
    InterlockedIncrement64(&m_shared.rejected);
    return std::nullopt;
}
void Mailbox::cancel(size_t slot) {
    check(m_mutexes[slot]->ReleaseSync(0));
    InterlockedExchange(&m_shared.slots[slot].state, Free);
}
void Mailbox::publish(size_t i, ID3D11Texture2D* source, uint64_t presentation,
    uint64_t timestamp, const FfiFrameWorkerSource& metadata) {
    auto& slot = m_shared.slots[i];
    m_context->CopyResource(m_textures[i].Get(), source); m_context->Flush();
    check(m_mutexes[i]->ReleaseSync(1));
    slot.serial = ++m_serial; slot.presentation = presentation; slot.timestamp = timestamp;
    slot.metadata = metadata; slot.queuedUs = now_us();
    InterlockedIncrement64(&m_shared.staged);
    InterlockedExchange(&slot.state, Ready);
}
std::optional<size_t> Mailbox::newest(uint64_t afterSerial) {
    // Prefer the newest GPU-complete image; a newer pending copy must not
    // hide an older completed image. Only the single consumer changes Ready.
    std::array<size_t, SlotCount> order{0, 1, 2};
    std::array<uint64_t, SlotCount> serial{};
    for (size_t i = 0; i < SlotCount; ++i)
        if (load(m_shared.slots[i].state) == Ready) serial[i] = m_shared.slots[i].serial;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return serial[a] > serial[b]; });
    for (const auto i : order) {
        if (serial[i] <= afterSerial) continue;
        if (m_mutexes[i]->AcquireSync(1, 0) != S_OK) continue;
        InterlockedExchange(&m_shared.slots[i].state, Reading);
        for (size_t older = 0; older < SlotCount; ++older) {
            if (load(m_shared.slots[older].state) != Ready
                || m_shared.slots[older].serial >= serial[i]) continue;
            if (m_mutexes[older]->AcquireSync(1, 0) != S_OK) continue;
            check(m_mutexes[older]->ReleaseSync(0));
            InterlockedExchange(&m_shared.slots[older].state, Free);
            InterlockedIncrement64(&m_shared.discarded);
        }
        return i;
    }
    return std::nullopt;
}
void Mailbox::finish(size_t slot) {
    check(m_mutexes[slot]->ReleaseSync(0));
    InterlockedExchange(&m_shared.slots[slot].state, Free);
}
}
