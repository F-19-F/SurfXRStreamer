#pragma once
#include "windows.hpp"
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace surfxr::transport::desktop {
struct ImeEventWindow {
    HWND window=nullptr,foreground=nullptr,focus=nullptr;
    DWORD process=0,thread=0;
};
// Metadata only. The observer never reads text, pixels or input events.
class ImeEventState {
public:
    void observe(DWORD event,const ImeEventWindow& window){
        if(event==EVENT_SYSTEM_FOREGROUND){entries.clear();return;}
        if(event!=EVENT_OBJECT_IME_SHOW&&event!=EVENT_OBJECT_IME_CHANGE&&
           event!=EVENT_OBJECT_IME_HIDE&&event!=EVENT_OBJECT_HIDE&&event!=EVENT_OBJECT_DESTROY)return;
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto& item){return item.window==window.window;}),entries.end());
        if((event==EVENT_OBJECT_IME_SHOW||event==EVENT_OBJECT_IME_CHANGE)&&window.window&&window.foreground&&window.process&&window.thread){
            if(entries.size()==16)entries.erase(entries.begin());
            entries.push_back(window);
        }
    }
    std::vector<ImeEventWindow> entries;
};
class ImeObserver {
public:
    ImeObserver();
    ~ImeObserver();
    std::vector<ImeEventWindow> windows();
    bool ready()const{return ready_.load();}
private:
    void run();
    static void CALLBACK event(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD);
    static thread_local ImeObserver* current_;
    std::atomic_bool stop_{false},ready_{false};std::mutex mutex_;ImeEventState state_;std::thread worker_;
};
HWND ime_focus(HWND foreground);
bool ime_event_current(const ImeEventWindow&,HWND foreground);
}
