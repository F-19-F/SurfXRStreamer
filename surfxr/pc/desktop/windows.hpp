#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wrl/client.h>
#include "surfxr/surfxr_desktop.hpp"
#include <memory>
#include <atomic>
#include <functional>

namespace surfxr::transport::desktop {
template<class T> using Com=Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT result,const char* error){require(SUCCEEDED(result),error);}
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
// Each capture/input thread owns its desktop handle and must attach before
// creating window-dependent resources. Never disable Windows secure desktop.
class InputDesktop {
public:
    ~InputDesktop(){if(desktop_){SetThreadDesktop(original_);CloseDesktop(desktop_);}}
    bool changed()const;
    bool attach();
private:
    HDESK original_=GetThreadDesktop(GetCurrentThreadId()),desktop_=nullptr;std::wstring name_;
};
struct Options {
    std::string bind="0.0.0.0",key_file,stop_event;
    unsigned port=control_port,fps=60,bitrate=40,monitor=0;
    bool service=false,agent=false,install=false,uninstall=false,self_test=false;
    bool dashboard_managed=false;
    // In-process module mode delegates transport and policy to its daemon.
    bool embedded=false,allow_input=true;
    std::function<void(const Bytes&)> send_video;
    std::function<unsigned(unsigned)> monitors;
    std::function<bool()> prefer_pc_mouse;
};
int run_host(const Options&,const std::atomic_bool& stop);
int service_main(const Options&);
int install_service(const Options&);
int uninstall_service();
void enable_system_privileges();
}
