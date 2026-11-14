#pragma once
#include "windows.hpp"
#include <cstdio>
#include <chrono>

namespace surfxr::transport::desktop {
// Hidden SYSTEM workers have no console. Only local check identifiers and
// numeric Win32 results belong in the Application log; never input contents.
inline void report_event(const char* code,DWORD error=0,WORD type=EVENTLOG_ERROR_TYPE,const char* detail=nullptr)noexcept{
    HANDLE source=RegisterEventSourceW(nullptr,L"SurfXRDesktop");
    if(!source)return;
    char number[16]{};std::snprintf(number,sizeof(number),"%lu",static_cast<unsigned long>(error));
    const char* strings[]{code,number,detail};ReportEventA(source,type,0,1,nullptr,detail?3:2,0,strings,nullptr);
    DeregisterEventSource(source);
}
inline void report_failure(const std::exception& error,const char* fallback)noexcept{
    std::string code=error.what();
    if(code.empty()||code.size()>96||code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")!=std::string::npos)code=fallback;
    report_event(code.c_str());
}
inline void report_duration(const char* code,std::chrono::steady_clock::time_point started)noexcept{
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    report_event(code,static_cast<DWORD>(ms>120000?120000:ms),EVENTLOG_INFORMATION_TYPE);
}
}
