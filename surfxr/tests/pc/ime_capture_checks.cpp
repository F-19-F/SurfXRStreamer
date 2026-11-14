#include "ime_capture.hpp"
#include "ime_events.hpp"
#include "ime_click.hpp"
#include "ime_uia.hpp"
#include "surfxr/keyboard_image.hpp"
#include <iostream>

namespace surfxr::transport::desktop {
namespace {
std::wstring probe_class(unsigned parent){return L"FrameInputPopupProbe-"+std::to_wstring(parent);}
unsigned downs=0,ups=0,invokes=0;
LRESULT CALLBACK popup(HWND window,UINT message,WPARAM w,LPARAM l){
    if(message==WM_LBUTTONDOWN){if(w==MK_LBUTTON&&LOWORD(l)==64&&HIWORD(l)==32)++downs;return 0;}
    if(message==WM_LBUTTONUP){if(w==0&&LOWORD(l)==64&&HIWORD(l)==32)++ups;return 0;}
    if(message==WM_COMMAND&&HIWORD(w)==BN_CLICKED){++invokes;return 0;}
    if(message==WM_APP+3){return CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,32,16,64,32,
        window,reinterpret_cast<HMENU>(1),GetModuleHandleW(nullptr),nullptr)!=nullptr;}
    if(message==WM_APP+4)return downs|(ups<<8)|(invokes<<16);
    if(message==WM_PAINT){PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        const auto brush=CreateSolidBrush(RGB(24,160,88));FillRect(dc,&rect,brush);DeleteObject(brush);EndPaint(window,&paint);return 0;}
    if(message==WM_APP+1){RECT rect{};GetWindowRect(window,&rect);SetWindowPos(window,nullptr,rect.left+32,rect.top+16,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        NotifyWinEvent(EVENT_OBJECT_IME_CHANGE,window,OBJID_WINDOW,CHILDID_SELF);return 0;}
    if(message==WM_APP+2){ShowWindow(window,SW_HIDE);NotifyWinEvent(EVENT_OBJECT_IME_HIDE,window,OBJID_WINDOW,CHILDID_SELF);return 0;}
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
}
int ime_probe_window(unsigned parent){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InputDesktop desktop;desktop.attach();
    const auto owner=GetForegroundWindow();RECT rect{};require(owner&&GetWindowRect(owner,&rect),"ime_probe_foreground_required");
    const auto cls=probe_class(parent);WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=popup;wc.lpszClassName=cls.c_str();
    require(RegisterClassW(&wc),"ime_probe_register");
    MONITORINFO monitor{sizeof(monitor)};require(GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor),"ime_probe_monitor");
    const auto x=std::clamp(rect.left+64,monitor.rcWork.left,monitor.rcWork.right-192);
    const auto y=std::clamp(rect.top+64,monitor.rcWork.top,monitor.rcWork.bottom-112);
    const auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,cls.c_str(),L"",WS_POPUP,x,y,128,64,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window){std::cout<<"{\"create_window_error\":"<<GetLastError()<<"}\n";fail("ime_probe_window");}ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
    // Arbitrary class and a different process; no IME/vendor executable name.
    NotifyWinEvent(EVENT_OBJECT_IME_SHOW,window,OBJID_WINDOW,CHILDID_SELF);
    const auto deadline=GetTickCount64()+10000;MSG message{};bool done=false;
    while(!done&&GetTickCount64()<deadline){
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT){done=true;break;}TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    if(IsWindow(window))DestroyWindow(window);UnregisterClassW(cls.c_str(),wc.hInstance);return 0;
}
int ime_capture_synthetic_check(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InputDesktop desktop;desktop.attach();ImeObserver observer;
    const auto hook_deadline=GetTickCount64()+2000;
    while(!observer.ready()&&GetTickCount64()<hook_deadline)Sleep(10);
    require(observer.ready(),"ime_probe_observer_unavailable");
    wchar_t exe[32768]{};require(GetModuleFileNameW(nullptr,exe,32768),"ime_probe_executable");
    auto command=L"\""+std::wstring(exe)+L"\" --ime-probe-window "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    require(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"ime_probe_child");
    Handle child(process.hProcess),thread(process.hThread);
    struct Reap {HANDLE child;HWND window=nullptr;~Reap(){if(window)PostMessageW(window,WM_CLOSE,0,0);if(WaitForSingleObject(child,1000)==WAIT_TIMEOUT){TerminateProcess(child,1);WaitForSingleObject(child,1000);}}}reap{child.value};
    const auto cls=probe_class(GetCurrentProcessId());ImeCandidate first;unsigned capture_status=0;bool observed=false;
    const auto deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline){
        reap.window=FindWindowExW(nullptr,nullptr,cls.c_str(),nullptr);
        if(reap.window){
            const auto entries=observer.windows();
            if(std::any_of(entries.begin(),entries.end(),[&](const auto& item){return item.window==reap.window;})){
                observed=true;first=capture_ime_candidate(&capture_status,&observer);if(first.window==reap.window&&!first.image.empty())break;
            }
        }
        Sleep(20);
    }
    if(first.window!=reap.window||!reap.window||first.discovery!=1||first.image.empty()){
        DWORD exit=0;GetExitCodeProcess(child.value,&exit);
        std::cout<<"{\"window_found\":"<<bool(reap.window)<<",\"event_observed\":"<<observed<<",\"capture_status\":"<<capture_status<<",\"discovery\":"<<first.discovery<<",\"child_exit\":"<<exit<<"}\n";
        fail("ime_probe_cross_process_capture");
    }
    const auto image=keyboard_image::decode(first.image.data(),first.image.size());
    require(image&&image->width==128&&image->height==64,"ime_probe_capture_size");
    for(std::size_t i=0;i<image->rgba.size();i+=4)require(image->rgba[i]==24&&image->rgba[i+1]==160&&image->rgba[i+2]==88,"ime_probe_synthetic_pixels");
    require(first.contains(.5f,.5f),"ime_probe_click_hit");
    require(ime_window_click(first,.5f,.5f),"ime_probe_direct_click");
    DWORD_PTR counts=0;
    const auto query=[&]{return SendMessageTimeoutW(reap.window,WM_APP+4,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&counts)!=0;};
    require(query()&&counts==0x101,"ime_probe_direct_click_pair");
    DWORD_PTR created=0;require(SendMessageTimeoutW(reap.window,WM_APP+3,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&created)&&created,"ime_probe_button");
    require(ime_uia_invoke(first,.5f,.5f)==ImeAction::Invoked,"ime_probe_uia_invoke");
    const auto invoke_deadline=GetTickCount64()+1500;
    while(query()&&(counts>>16)==0&&GetTickCount64()<invoke_deadline)Sleep(10);
    require(query()&&counts==0x10101&&GetForegroundWindow()==first.foreground,"ime_probe_uia_commit_without_activation");
    PostMessageW(reap.window,WM_APP+1,0,0);ImeCandidate moved;
    const auto move_deadline=GetTickCount64()+1500;
    while(GetTickCount64()<move_deadline){moved=capture_ime_candidate(nullptr,&observer);if(moved.window==first.window&&moved.rect.left==first.rect.left+32)break;Sleep(20);}
    require(moved.window==first.window&&moved.discovery==1&&!first.same_target(moved)&&!first.contains(.5f,.5f)&&moved.contains(.5f,.5f),"ime_probe_move_and_stale_click");
    PostMessageW(reap.window,WM_APP+2,0,0);bool hidden=false;
    const auto hide_deadline=GetTickCount64()+1500;
    while(GetTickCount64()<hide_deadline){const auto entries=observer.windows();
        hidden=!IsWindowVisible(reap.window)&&std::none_of(entries.begin(),entries.end(),[&](const auto& item){return item.window==reap.window;});if(hidden)break;Sleep(20);}
    require(hidden&&!moved.contains(.5f,.5f),"ime_probe_hide_and_stale_click");
    std::cout<<"{\"success\":true,\"cross_process\":true,\"standard_ime_events\":true,\"synthetic_pixels_verified\":true,\"move_and_hide\":true,\"hit_testing\":true,\"direct_click_pair\":true,\"uia_invoke\":true,\"foreground_preserved\":true,\"global_input_injected\":false,\"media_saved\":false}\n";
    return 0;
}
}
