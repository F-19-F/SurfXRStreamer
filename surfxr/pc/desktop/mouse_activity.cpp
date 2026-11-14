#include "mouse_activity.hpp"
#include <future>

namespace surfxr::transport::desktop {
thread_local MouseActivity* MouseActivity::current_=nullptr;
bool MouseActivity::start(){
    if(running_.load())return true;
    stop();std::promise<bool> ready;auto result=ready.get_future();
    thread_=std::thread([this,ready=std::move(ready)]()mutable{
        InputDesktop desktop;HHOOK handle=nullptr;MSG message{};
        try{
            desktop.attach();
            positioned_=GetCursorPos(&cursor_)!=FALSE;
            // Create the thread queue before publishing its ID to stop().
            PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
            thread_id_=GetCurrentThreadId();current_=this;
            HMODULE module=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&MouseActivity::hook),&module))
                handle=SetWindowsHookExW(WH_MOUSE_LL,hook,module,0);
        }catch(const std::exception&){}
        running_.store(handle!=nullptr);ready.set_value(handle!=nullptr);
        if(handle){
            while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
            UnhookWindowsHookEx(handle);
        }
        current_=nullptr;running_.store(false);
    });
    const bool ok=result.get();if(!ok)stop();return ok;
}
void MouseActivity::stop(){
    if(thread_.joinable()){
        if(running_.load())PostThreadMessageW(thread_id_,WM_QUIT,0,0);
        thread_.join();
    }
    thread_id_=0;
}
LRESULT CALLBACK MouseActivity::hook(int code,WPARAM kind,LPARAM data){
    if(code==HC_ACTION&&kind==WM_MOUSEMOVE&&current_){
        const auto& event=*reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        auto& self=*current_;
        if(event.dwExtraInfo==injection_tag){
            // Absolute VR injections change the native cursor baseline, but
            // must never count toward the PC's takeover displacement.
            self.cursor_=event.pt;self.positioned_=true;
        }else{
            const auto now=PointerArbiter::Clock::now();
            if(!self.positioned_){self.cursor_=event.pt;self.positioned_=true;}
            const double dx=double(event.pt.x)-self.cursor_.x;
            const double dy=double(event.pt.y)-self.cursor_.y;
            if(!self.arbiter_.pc_moved(dx,dy,now))return 1;
            self.cursor_=event.pt;self.positioned_=true;
            // Suppressed relative moves leave the OS cursor at its baseline.
            // Subsequent deltas must still measure from that actual position.
        }
    }
    return CallNextHookEx(nullptr,code,kind,data);
}
}
