#pragma once
#include "worker.hpp"
#include "keyboard.hpp"
#include "audio.hpp"
#include "desktop_module.hpp"
namespace surfxr::daemon {
using CapabilitiesPatch=int (*)(const std::uint8_t*,std::size_t,const std::uint8_t*,std::size_t,const std::uint8_t*,std::uint8_t*,std::size_t);
using SocketQos=void* (*)(std::uint64_t,bool);
using SocketQosStop=void (*)(void*);
class Runtime {
public:
    Runtime();~Runtime();
    DesktopRun desktop=nullptr;WorkerRunV7 worker=nullptr;AudioConfig audio=nullptr;MicrophoneRun microphone=nullptr;
    CapabilitiesPatch capabilities=nullptr;CandidateUpdate candidate_update=nullptr;
    SocketQos qos=nullptr;SocketQosStop qos_stop=nullptr;
private:
    HMODULE desktop_=nullptr,vr_=nullptr;
};
int run(const surfxr::transport::desktop::Options&,const std::atomic_bool&);
}
