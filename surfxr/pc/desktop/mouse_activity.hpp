#pragma once
#include "windows.hpp"
#include "pointer_arbiter.hpp"
#include <thread>

namespace surfxr::transport::desktop {
// A dedicated message thread prevents network waits from delaying local input
// or timing out the Windows low-level hook. All sharing is bounded atomics.
class MouseActivity {
public:
    static constexpr ULONG_PTR injection_tag=0x414c5652;
    explicit MouseActivity(PointerArbiter& arbiter):arbiter_(arbiter){}
    ~MouseActivity(){stop();}
    bool start();
    void stop();
private:
    static LRESULT CALLBACK hook(int,WPARAM,LPARAM);
    static thread_local MouseActivity* current_;
    PointerArbiter& arbiter_;
    std::thread thread_;std::atomic_bool running_{false};
    DWORD thread_id_=0;
    POINT cursor_{};bool positioned_=false;
};
}
