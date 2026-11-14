#pragma once
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
namespace surfxr {
// Winsock waits use this process's timer resolution, independently of QPC.
class WindowsTimerPeriod {
    bool active_=timeBeginPeriod(1)==TIMERR_NOERROR;
public:
    WindowsTimerPeriod()=default;
    ~WindowsTimerPeriod(){if(active_)timeEndPeriod(1);}
    WindowsTimerPeriod(const WindowsTimerPeriod&)=delete;
    WindowsTimerPeriod& operator=(const WindowsTimerPeriod&)=delete;
    bool active()const noexcept{return active_;}
};
}
#endif
