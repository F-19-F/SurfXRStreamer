#include "ime_events.hpp"
#include <array>

namespace surfxr::transport::desktop {
thread_local ImeObserver* ImeObserver::current_=nullptr;
HWND ime_focus(HWND foreground){
    GUITHREADINFO info{sizeof(info)};
    return foreground&&GetGUIThreadInfo(GetWindowThreadProcessId(foreground,nullptr),&info)?info.hwndFocus:nullptr;
}
bool ime_event_current(const ImeEventWindow& item,HWND foreground){
    DWORD process=0;const auto thread=GetWindowThreadProcessId(item.window,&process);
    return item.foreground==foreground&&item.focus==ime_focus(foreground)&&
        item.process==process&&item.thread==thread&&IsWindowVisible(item.window)&&!IsIconic(item.window);
}
ImeObserver::ImeObserver():worker_([this]{run();}){}
ImeObserver::~ImeObserver(){stop_.store(true);worker_.join();}
std::vector<ImeEventWindow> ImeObserver::windows(){std::lock_guard<std::mutex> lock(mutex_);return state_.entries;}
void CALLBACK ImeObserver::event(HWINEVENTHOOK,DWORD code,HWND window,LONG object,LONG child,DWORD thread,DWORD){
    if(!current_||child!=CHILDID_SELF||(object!=OBJID_WINDOW&&object!=OBJID_CLIENT))return;
    ImeEventWindow item;item.window=window;item.foreground=GetForegroundWindow();item.focus=ime_focus(item.foreground);
    const auto actual=GetWindowThreadProcessId(window,&item.process);item.thread=actual;
    if((code==EVENT_OBJECT_IME_SHOW||code==EVENT_OBJECT_IME_CHANGE)&&(!actual||actual!=thread))return;
    std::lock_guard<std::mutex> lock(current_->mutex_);current_->state_.observe(code,item);
}
void ImeObserver::run(){
    // Out-of-context WinEvents require a message pump. Keep it off the input
    // owner so attaching to a different input desktop cannot strand hooks.
    current_=this;InputDesktop desktop;
    while(!stop_.load()){
        try{
            desktop.attach();std::array<HWINEVENTHOOK,3> hooks{};
            struct Unhook {std::array<HWINEVENTHOOK,3>& hooks;~Unhook(){for(auto hook:hooks)if(hook)UnhookWinEvent(hook);}}unhook{hooks};
            hooks[0]=SetWinEventHook(EVENT_OBJECT_IME_SHOW,EVENT_OBJECT_IME_CHANGE,nullptr,event,0,0,WINEVENT_OUTOFCONTEXT);
            hooks[1]=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_HIDE,nullptr,event,0,0,WINEVENT_OUTOFCONTEXT);
            hooks[2]=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,event,0,0,WINEVENT_OUTOFCONTEXT);
            require(hooks[0]&&hooks[1]&&hooks[2],"ime_event_hook_failed");
            ready_.store(true);
            auto check_desktop=std::chrono::steady_clock::now();
            while(!stop_.load()){
                if(std::chrono::steady_clock::now()>=check_desktop){
                    if(desktop.changed())break;check_desktop=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
                }
                MSG message{};for(unsigned i=0;i<64&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++i){TranslateMessage(&message);DispatchMessageW(&message);}
                MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            }
        }catch(const std::exception&){}
        ready_.store(false);
        {std::lock_guard<std::mutex> lock(mutex_);state_.entries.clear();}
        if(!stop_.load())Sleep(100);
    }
    current_=nullptr;
}
}
