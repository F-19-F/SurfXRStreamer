#include "network.hpp"
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace surfxr::transport::desktop {
namespace {
namespace fs=std::filesystem;
constexpr wchar_t name[]=L"SurfXRDesktop";
struct ServiceHandle {SC_HANDLE value;explicit ServiceHandle(SC_HANDLE h):value(h){require(value,"desktop_service_manager_access_denied");}~ServiceHandle(){CloseServiceHandle(value);}ServiceHandle(const ServiceHandle&)=delete;};
void protect(const fs::path& path,bool users_read=false){
    require(!(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"desktop_install_reparse_point_rejected");
    PSECURITY_DESCRIPTOR security=nullptr;const auto sddl=users_read?L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)":L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&security,nullptr),"desktop_install_acl_parse");
    const BOOL success=SetFileSecurityW(path.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,security);LocalFree(security);require(success,"desktop_install_acl_failed");
}
fs::path folder(REFKNOWNFOLDERID id){PWSTR path=nullptr;check(SHGetKnownFolderPath(id,0,nullptr,&path),"desktop_install_folder");fs::path result(path);CoTaskMemFree(path);return result;}
}
int install_service(const Options& o){
    const auto root=o.dashboard_managed?Key{}:read_key(o.key_file);ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CREATE_SERVICE));
    // Reject replacing a running host; building/installing never silently
    // interrupts an existing desktop or VR session.
    SC_HANDLE prior=OpenServiceW(manager.value,name,SERVICE_QUERY_STATUS);if(prior){SERVICE_STATUS s{};const BOOL success=QueryServiceStatus(prior,&s);CloseServiceHandle(prior);require(success&&s.dwCurrentState==SERVICE_STOPPED,"desktop_service_must_be_stopped_before_update");}
    auto target=folder(FOLDERID_ProgramFiles)/L"SurfXRDesktop",data=folder(FOLDERID_ProgramData)/L"SurfXRDesktop";
    fs::create_directories(target);protect(target,true);fs::create_directories(data);protect(data);
    wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)>0,"desktop_install_executable");const auto source=fs::path(executable).parent_path();
    for(const auto& entry:fs::directory_iterator(source))if(entry.is_regular_file()&&(entry.path().filename()==L"surfxr-desktop.exe"||entry.path().filename()==L"surfxr-daemon.exe"||entry.path().filename()==L"FFmpeg.LICENSE.txt"||entry.path().extension()==L".dll")){
        auto destination=target/entry.path().filename();if(fs::exists(destination))protect(destination,true);if(!fs::equivalent(source,target))fs::copy_file(entry.path(),destination,fs::copy_options::overwrite_existing);protect(destination,true);
    }
    const auto key_file=data/L"desktop.key";
    if(!o.dashboard_managed){if(fs::exists(key_file))protect(key_file);std::ofstream key(key_file,std::ios::binary|std::ios::trunc);require(bool(key),"desktop_install_key_create");constexpr char hex[]="0123456789abcdef";for(auto b:root)key<<hex[b>>4]<<hex[b&15];key<<'\n';key.close();require(bool(key),"desktop_install_key_write");protect(key_file);}
    in_addr address{};require(inet_pton(AF_INET,o.bind.c_str(),&address)==1,"desktop_bind_ipv4");
    const auto command=L"\""+(target/fs::path(executable).filename()).wstring()+L"\" --service "+(o.dashboard_managed?L"--dashboard-managed":L"--key-file \""+key_file.wstring()+L"\"")+L" --bind "+fs::path(o.bind).wstring()+L" --port "+std::to_wstring(o.port)+L" --fps "+std::to_wstring(o.fps)+L" --bitrate "+std::to_wstring(o.bitrate)+L" --monitor "+std::to_wstring(o.monitor);
    SC_HANDLE service=OpenServiceW(manager.value,name,SERVICE_CHANGE_CONFIG);if(service){ServiceHandle existing(service);require(ChangeServiceConfigW(existing.value,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,SERVICE_NO_CHANGE,command.c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr,L"Steam Frame SURFXR Desktop"),"desktop_service_update_failed");}
    else {ServiceHandle created(CreateServiceW(manager.value,name,L"Steam Frame SURFXR Desktop",SERVICE_CHANGE_CONFIG,SERVICE_WIN32_OWN_PROCESS,SERVICE_DEMAND_START,SERVICE_ERROR_NORMAL,command.c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr));}
    std::cout<<"Service installed and stopped. Start SurfXRDesktop explicitly when ready. No firewall rules or UAC policies were changed.\n";return 0;
}
int uninstall_service(){ServiceHandle manager(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));ServiceHandle service(OpenServiceW(manager.value,name,DELETE|SERVICE_QUERY_STATUS));SERVICE_STATUS status{};require(QueryServiceStatus(service.value,&status)&&status.dwCurrentState==SERVICE_STOPPED,"desktop_service_must_be_stopped_before_uninstall");require(DeleteService(service.value),"desktop_service_delete_failed");std::cout<<"Service removed. Installed files and pairing key retained.\n";return 0;}
}
