#include "runtime.hpp"
#include <filesystem>
namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
Runtime::Runtime(){
    wchar_t path[32768]{};require(GetModuleFileNameW(nullptr,path,32768)>0,"daemon_executable_path");const auto root=std::filesystem::path(path).parent_path();
    desktop_=LoadLibraryExW((root/L"surfxr-desktop-module.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);require(desktop_,"daemon_desktop_module_missing");
    vr_=LoadLibraryExW((root/L"driver_surfxr_server.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!vr_){FreeLibrary(desktop_);desktop_=nullptr;fail("daemon_vr_module_missing");}
    desktop=reinterpret_cast<DesktopRun>(GetProcAddress(desktop_,"frame_desktop_session"));worker=reinterpret_cast<WorkerRunV7>(GetProcAddress(vr_,"FrameDaemonWorkerRunV7"));
    audio=reinterpret_cast<AudioConfig>(GetProcAddress(vr_,"FrameDaemonAudioConfig"));microphone=reinterpret_cast<MicrophoneRun>(GetProcAddress(vr_,"FrameDaemonMicrophone"));
    candidate_update=reinterpret_cast<CandidateUpdate>(GetProcAddress(vr_,"FrameDaemonKeyboardImageV1"));
    capabilities=reinterpret_cast<CapabilitiesPatch>(GetProcAddress(vr_,"FrameDaemonCapabilities"));
    qos=reinterpret_cast<SocketQos>(GetProcAddress(vr_,"FrameDaemonSocketQos"));qos_stop=reinterpret_cast<SocketQosStop>(GetProcAddress(vr_,"FrameDaemonSocketQosStop"));
    if(!desktop||!worker||!audio||!microphone||!capabilities||!qos||!qos_stop){FreeLibrary(vr_);FreeLibrary(desktop_);vr_=desktop_=nullptr;fail("daemon_module_version_mismatch");}
}
Runtime::~Runtime(){if(vr_)FreeLibrary(vr_);if(desktop_)FreeLibrary(desktop_);}
}
