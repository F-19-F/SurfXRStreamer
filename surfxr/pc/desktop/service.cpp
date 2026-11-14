#include "windows.hpp"
#include <wtsapi32.h>
#include <userenv.h>
#include <filesystem>
#include <thread>
#include <sddl.h>

namespace surfxr::transport::desktop {
namespace {
std::atomic_bool stopped{false};SERVICE_STATUS_HANDLE status_handle=nullptr;const Options* options=nullptr;
constexpr wchar_t service_name[]=L"SurfXRDesktop";
void status(DWORD state,DWORD error=0){SERVICE_STATUS s{SERVICE_WIN32_OWN_PROCESS,state,SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN,error,0,0,0};if(state==SERVICE_START_PENDING||state==SERVICE_STOP_PENDING){s.dwControlsAccepted=0;s.dwWaitHint=5000;}SetServiceStatus(status_handle,&s);}
void WINAPI control(DWORD request){if(request==SERVICE_CONTROL_STOP||request==SERVICE_CONTROL_SHUTDOWN){stopped.store(true);status(SERVICE_STOP_PENDING);}}
std::wstring quote(const std::string& text){require(text.find_first_of("\"\r\n")==std::string::npos,"desktop_service_argument_invalid");auto path=std::filesystem::path(text).wstring();return L"\""+path+L"\"";}
void WINAPI entry(DWORD,wchar_t**){
    status_handle=RegisterServiceCtrlHandlerW(service_name,control);if(!status_handle)return;status(SERVICE_START_PENDING);
    try{
        enable_system_privileges();wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)>0,"desktop_service_executable");
        const std::wstring event_name=L"Global\\SurfXRDesktopStop-"+std::to_wstring(GetCurrentProcessId());PSECURITY_DESCRIPTOR security=nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",SDDL_REVISION_1,&security,nullptr),"desktop_stop_event_acl");SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};Handle event(CreateEventW(&attributes,TRUE,FALSE,event_name.c_str()));const auto event_error=GetLastError();LocalFree(security);require(event.value&&event_error!=ERROR_ALREADY_EXISTS,"desktop_stop_event_create");
        const auto& o=*options;const std::wstring args=L"\""+std::wstring(executable)+L"\" --agent --stop-event \""+event_name+L"\" "+(o.dashboard_managed?L"--dashboard-managed":L"--key-file "+quote(o.key_file))+L" --bind "+quote(o.bind)+L" --port "+std::to_wstring(o.port)+L" --fps "+std::to_wstring(o.fps)+L" --bitrate "+std::to_wstring(o.bitrate)+L" --monitor "+std::to_wstring(o.monitor);
        HANDLE process=nullptr;DWORD session=MAXDWORD;auto retry=std::chrono::steady_clock::now();status(SERVICE_RUNNING);
        while(!stopped.load()){
            const auto active=WTSGetActiveConsoleSessionId();
            if(process&&(active!=session||WaitForSingleObject(process,0)==WAIT_OBJECT_0)){SetEvent(event.value);if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0)TerminateProcess(process,0);CloseHandle(process);process=nullptr;retry=std::chrono::steady_clock::now()+std::chrono::seconds(2);}
            if(!process&&active!=MAXDWORD&&active!=0&&std::chrono::steady_clock::now()>=retry){
                Handle original;require(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&original.value),"desktop_service_token");Handle token;
                require(DuplicateTokenEx(original.value,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&token.value),"desktop_service_duplicate_token");
                DWORD target=active;require(SetTokenInformation(token.value,TokenSessionId,&target,sizeof(target)),"desktop_service_session_token");
                ResetEvent(event.value);STARTUPINFOW startup{};startup.cb=sizeof(startup);wchar_t desktop[]=L"winsta0\\default";startup.lpDesktop=desktop;PROCESS_INFORMATION child{};auto command=args;
                if(CreateProcessAsUserW(token.value,executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,std::filesystem::path(executable).parent_path().c_str(),&startup,&child)){
                    process=child.hProcess;CloseHandle(child.hThread);session=active;
                }retry=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if(process){SetEvent(event.value);if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0){TerminateProcess(process,0);WaitForSingleObject(process,2000);}CloseHandle(process);}status(SERVICE_STOPPED);
    }catch(...){status(SERVICE_STOPPED,ERROR_SERVICE_SPECIFIC_ERROR);}
}
}
void enable_system_privileges(){
    Handle token;require(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token.value),"desktop_service_privilege_token");
    for(const wchar_t* privilege:{L"SeTcbPrivilege",L"SeAssignPrimaryTokenPrivilege",L"SeIncreaseQuotaPrivilege"}){TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;require(LookupPrivilegeValueW(nullptr,privilege,&p.Privileges[0].Luid),"desktop_service_privilege_lookup");SetLastError(ERROR_SUCCESS);require(AdjustTokenPrivileges(token.value,FALSE,&p,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS,"desktop_service_requires_local_system");}
}
int service_main(const Options& o){options=&o;SERVICE_TABLE_ENTRYW entries[]{{const_cast<wchar_t*>(service_name),entry},{nullptr,nullptr}};require(StartServiceCtrlDispatcherW(entries),"desktop_service_dispatch_failed");return 0;}
}
