#pragma once
#include <cstddef>
#include <cstdint>

// A versioned C boundary: the daemon owns authentication, sockets and audio.
// Callbacks are synchronous and may not retain the module's byte buffers.
namespace surfxr::daemon {
constexpr std::uint32_t desktop_module_version=2;
struct DesktopSession {
    std::uint32_t bytes=sizeof(DesktopSession),version=desktop_module_version;
    void* owner=nullptr;
    unsigned fps=60,bitrate=40,monitor=0;
    bool input=true,multimonitor=true;
    bool (*stopped)(void*)=nullptr;
    bool (*readable)(void*,unsigned)=nullptr;
    int (*receive)(void*,std::uint8_t*,std::size_t)=nullptr;
    bool (*control)(void*,const std::uint8_t*,std::size_t)=nullptr;
    bool (*video)(void*,const std::uint8_t*,std::size_t)=nullptr;
    unsigned (*monitors)(void*,unsigned)=nullptr;
    bool (*prefer_pc_mouse)(void*)=nullptr;
};
using DesktopRun=int (*)(const DesktopSession*);
}
