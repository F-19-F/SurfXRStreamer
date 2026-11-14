#pragma once
#include "FrameWorkerShared.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <optional>
#include <wrl/client.h>
#include <array>

namespace frame_worker {
class Mailbox {
public:
    Mailbox(Shared& shared, ID3D11Device* device, ID3D11Texture2D* source);
    Mailbox(Shared& shared, ID3D11Device* device);
    ~Mailbox();
    std::optional<size_t> begin_capture();
    void cancel(size_t slot);
    void publish(size_t slot, ID3D11Texture2D* source, uint64_t presentation,
                 uint64_t timestamp, const FfiFrameWorkerSource& metadata);
    std::optional<size_t> newest(uint64_t afterSerial);
    void finish(size_t slot);
    ID3D11Texture2D* texture(size_t slot) const { return m_textures[slot].Get(); }
    std::array<HANDLE, SlotCount> handles() const { return m_handles; }
private:
    Shared& m_shared;
    bool m_producer;
    uint64_t m_serial = 0;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, SlotCount> m_textures;
    std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, SlotCount> m_mutexes;
    std::array<HANDLE, SlotCount> m_handles{};
};
}
