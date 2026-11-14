#include "ime_click.hpp"
#include "ime_uia.hpp"
#include "input.hpp"
#include "diagnostics.hpp"
#include <cmath>
#include <climits>

namespace surfxr::transport::desktop {
bool ime_pointer_constrained(HWND foreground,HWND candidate){
    GUITHREADINFO info{sizeof(info)};
    const auto thread=GetWindowThreadProcessId(foreground,nullptr);
    if(thread&&GetGUIThreadInfo(thread,&info)&&info.hwndCapture&&
       info.hwndCapture!=candidate&&!IsChild(candidate,info.hwndCapture))return true;
    RECT clip{};
    const int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const RECT desktop{x,y,x+GetSystemMetrics(SM_CXVIRTUALSCREEN),y+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if(GetClipCursor(&clip)&&!EqualRect(&clip,&desktop))return true;
    // Some games warp the pointer every frame without exposing mouse capture.
    MONITORINFO monitor{sizeof(monitor)};RECT app{};
    return GetWindowRect(foreground,&app)&&GetMonitorInfoW(MonitorFromWindow(foreground,MONITOR_DEFAULTTONEAREST),&monitor)&&
        app.left<=monitor.rcMonitor.left&&app.top<=monitor.rcMonitor.top&&
        app.right>=monitor.rcMonitor.right&&app.bottom>=monitor.rcMonitor.bottom;
}
bool ime_window_click(const ImeCandidate& candidate,float u,float v){
    const auto prior=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    struct Restore {DPI_AWARENESS_CONTEXT prior;~Restore(){if(prior)SetThreadDpiAwarenessContext(prior);}}restore{prior};
    if(!candidate.contains(u,v))return false;
    POINT point{candidate.rect.left+static_cast<LONG>(std::lround(u*(candidate.rect.right-candidate.rect.left-1))),
        candidate.rect.top+static_cast<LONG>(std::lround(v*(candidate.rect.bottom-candidate.rect.top-1)))};
    const auto target=WindowFromPoint(point);
    if(target!=candidate.window&&!IsChild(candidate.window,target))return false;
    DWORD process=0;const auto thread=GetWindowThreadProcessId(target,&process);
    if(!thread||!ScreenToClient(target,&point)||point.x<SHRT_MIN||point.x>SHRT_MAX||point.y<SHRT_MIN||point.y>SHRT_MAX)return false;
    const auto position=MAKELPARAM(static_cast<WORD>(point.x),static_cast<WORD>(point.y));
    const auto send=[&](UINT message,WPARAM buttons){DWORD_PTR result=0;
        return SendMessageTimeoutW(target,message,buttons,position,SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,50,&result)!=0;};
    if(!send(WM_MOUSEMOVE,0)||!candidate.contains(u,v))return false;
    const bool down=send(WM_LBUTTONDOWN,MK_LBUTTON);
    // A failed/timed-out down may already have executed. Never replay it or
    // fall back to a global click; only send the matching up to the same owner.
    DWORD current_process=0;
    if(GetWindowThreadProcessId(target,&current_process)!=thread||current_process!=process)return down;
    const bool up=send(WM_LBUTTONUP,0);return down&&up;
}
void Input::click(const ImeCandidate& candidate,float u,float v){
    if(!tick()||!candidate.contains(u,v))return;
    if(!ime_pointer_constrained(candidate.foreground,candidate.window)){
        const auto& r=candidate.rect;click(Display{r.left,r.top,r.right-r.left,r.bottom-r.top,0,0},u,v);return;
    }
    const auto action=ime_uia_invoke(candidate,u,v);
    const bool accepted=action==ImeAction::Invoked||(action==ImeAction::Unavailable&&ime_window_click(candidate,u,v));
    const auto now=std::chrono::steady_clock::now();
    if(now>=next_report_){
        report_event(accepted?"desktop_ime_direct_click":"desktop_ime_direct_click_failed",static_cast<DWORD>(action),
            accepted?EVENTLOG_INFORMATION_TYPE:EVENTLOG_WARNING_TYPE);
        next_report_=now+std::chrono::seconds(5);
    }
}
}
