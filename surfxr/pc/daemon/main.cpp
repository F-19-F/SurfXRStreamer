#include "runtime.hpp"
#include "../desktop/windows.hpp"
#include "surfxr/surfxr_unified.hpp"
#include "surfxr/surfxr_unified_media.hpp"
#include <charconv>
#include <filesystem>
#include <iostream>

namespace {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
std::atomic_bool stopping{false};
BOOL WINAPI interrupt(DWORD signal){if(signal==CTRL_C_EVENT||signal==CTRL_BREAK_EVENT||signal==CTRL_CLOSE_EVENT){stopping.store(true);return TRUE;}return FALSE;}
unsigned number(const char* text,unsigned lo,unsigned hi){unsigned value=0;const auto end=text+std::strlen(text);const auto result=std::from_chars(text,end,value);require(result.ec==std::errc{}&&result.ptr==end&&value>=lo&&value<=hi,"daemon_option_value");return value;}
}
int main(int argc,char** argv){
    try{
        Options options;options.dashboard_managed=true;bool self_test=false;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];
            if(arg=="--host"||arg=="--dashboard-managed")continue;
            if(arg=="--service"){options.service=true;continue;}if(arg=="--agent"){options.agent=true;continue;}
            if(arg=="--install-service"){options.install=true;continue;}if(arg=="--uninstall-service"){options.uninstall=true;continue;}
            if(arg=="--self-test"){self_test=true;continue;}
            if(arg=="--help"){std::cout<<"surfxr-daemon --service --dashboard-managed\n--install-service / --uninstall-service manage the existing SurfXRDesktop SYSTEM service. --self-test loads modules and checks offline ABI/protocol only.\n";return 0;}
            require(i+1<argc,"daemon_missing_option_value");const auto value=argv[++i];
            if(arg=="--bind")options.bind=value;else if(arg=="--port")options.port=number(value,1024,65533);
            else if(arg=="--fps")options.fps=number(value,30,144);else if(arg=="--bitrate")options.bitrate=number(value,1,200);else if(arg=="--monitor")options.monitor=number(value,0,15);
            else if(arg=="--stop-event")options.stop_event=value;else fail("daemon_unknown_option");
        }
        if(options.service && options.port == 9950) {
            options.port=surfxr::ports::migrate_service_desktop(options.port);
            std::cerr<<"SurfXR service: migrated legacy desktop port to "<<options.port<<'\n';
        }
        if(self_test){
            const auto bytes=unified::record(unified::Kind::Begin,7,Bytes{0,1,2});const auto record=unified::parse(bytes);
            require(record.generation==7&&record.kind==unified::Kind::Begin&&record.body==Bytes({0,1,2}),"daemon_protocol_roundtrip");
            Key key{};unified::MediaCipher sender(key),receiver(key);sender.protection(unified::Protection::Plain);receiver.protection(unified::Protection::Plain);
            Bytes shard(1400,9),wire,scratch;sender.seal(7,shard.data(),shard.size(),wire);const auto view=receiver.open(wire.data(),wire.size(),scratch);
            Reader media(view.data,view.size);require(media.integer(4)==7&&media.remaining()==1400&&view.data==wire.data()+16,"daemon_plain_media_offline_contract");
            surfxr::daemon::Runtime runtime;require(runtime.desktop(nullptr)==2&&runtime.worker(0,0,nullptr,nullptr)==2&&runtime.audio(nullptr,0,nullptr,0)==0,"daemon_module_offline_contract");
            std::cout<<"Daemon offline: module exports and unified control framing passed. No capture, audio devices, IPC listeners, services or network.\n";return 0;
        }
        if(options.uninstall)return uninstall_service();if(options.install)return install_service(options);if(options.service)return service_main(options);
        SetConsoleCtrlHandler(interrupt,TRUE);Handle event;std::thread watcher;std::atomic_bool done{false};
        struct Join {std::atomic_bool& done;std::thread& thread;~Join(){done.store(true);if(thread.joinable())thread.join();}}join{done,watcher};
        if(!options.stop_event.empty()){
            require(options.agent&&options.stop_event.rfind("Global\\SurfXRDesktopStop-",0)==0,"daemon_stop_event_invalid");event.value=OpenEventW(SYNCHRONIZE,FALSE,std::filesystem::path(options.stop_event).c_str());require(event.value,"daemon_stop_event_unavailable");
            watcher=std::thread([&]{while(!done.load())if(WaitForSingleObject(event.value,50)==WAIT_OBJECT_0){stopping.store(true);break;}});
        }
        return surfxr::daemon::run(options,stopping);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
