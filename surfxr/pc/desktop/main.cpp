#include "network.hpp"
#include "../daemon/desktop_module.hpp"
#include "surfxr/surfxr_stream.hpp"
#include "capture.hpp"
#include "ime_capture.hpp"
#include "video_sender.hpp"
#include <charconv>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
#include <sddl.h>

namespace surfxr::transport::desktop {
void media_lease_self_test();
void dashboard_watch_self_test();
int ime_capture_synthetic_check();
int ime_probe_window(unsigned parent);
void desktop_color_self_test(unsigned monitor,const std::string& output_directory);
namespace {
std::atomic_bool stopping{false};
BOOL WINAPI interrupt(DWORD signal){if(signal==CTRL_C_EVENT||signal==CTRL_BREAK_EVENT||signal==CTRL_CLOSE_EVENT){stopping.store(true);return TRUE;}return FALSE;}
unsigned number(const char* text,unsigned lo,unsigned hi){unsigned value=0;auto end=text+std::strlen(text);auto r=std::from_chars(text,end,value);require(r.ec==std::errc{}&&r.ptr==end&&value>=lo&&value<=hi,"desktop_invalid_number");return value;}
int self_test(){
    wchar_t filename[32768]{};require(GetModuleFileNameW(nullptr,filename,32768)>0,"desktop_module_test_executable");
    const auto module_path=std::filesystem::path(filename).parent_path()/L"surfxr-desktop-module.dll";
    if(std::filesystem::exists(module_path)){
        HMODULE module=LoadLibraryExW(module_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module,"desktop_module_load_failed");
        const auto run=reinterpret_cast<surfxr::daemon::DesktopRun>(GetProcAddress(module,"frame_desktop_session"));
        const auto valid=run&&run(nullptr)==2;FreeLibrary(module);require(valid,"desktop_module_invalid_api_accepted");
    }
    video_sender_self_test();
    media_lease_self_test();
    dashboard_watch_self_test();
    Key zero{};Cipher reference(zero);const auto expected=reference.seal(Bytes{1,2,3,4});constexpr char reference_hex[]="0100000000000000e7d6fc0f589e3336167c6728da9239267bb3ba58";std::string rendered;constexpr char digits[]="0123456789abcdef";for(auto b:expected){rendered+=digits[b>>4];rendered+=digits[b&15];}require(rendered==reference_hex,"desktop_cipher_reference");
    Key root{};for(unsigned i=0;i<32;++i)root[i]=static_cast<std::uint8_t>(i);const auto server=random(),client=random();
    Key ones{};ones.fill(1);const auto known=proof(root,zero,ones,"ALVR-DESKTOP-client-proof-v1");rendered.clear();for(auto b:known){rendered+=digits[b>>4];rendered+=digits[b&15];}require(rendered=="bbbeea7b1e7244bba9c17a4c71ba9d831b035fc269335216e32f824a6ca2d042","desktop_hmac_reference");
    const auto key=proof(root,server,client,"ALVR-DESKTOP-control-c2s-v1");Cipher sender(key),receiver(key);auto record=sender.seal(Bytes{1,2,3,4});require(receiver.open(record)==Bytes({1,2,3,4}),"desktop_cipher_roundtrip");
    bool rejected=false;try{receiver.open(record);}catch(...){rejected=true;}require(rejected,"desktop_cipher_replay_accepted");
    auto next=sender.seal(Bytes{5,6});next.back()^=1;rejected=false;try{receiver.open(next);}catch(...){rejected=true;}require(rejected,"desktop_cipher_tampering_accepted");
    next.back()^=1;require(receiver.open(next)==Bytes({5,6}),"desktop_cipher_tamper_advanced_sequence");
    Cipher udp_sender(key),udp_receiver(key);const auto first=udp_sender.seal(Bytes{1}),second=udp_sender.seal(Bytes{2}),third=udp_sender.seal(Bytes{3});
    require(udp_receiver.open(third,false)==Bytes({3})&&udp_receiver.open(first,false)==Bytes({1})&&udp_receiver.open(second,false)==Bytes({2}),"desktop_udp_reorder");
    rejected=false;try{udp_receiver.open(second,false);}catch(...){rejected=true;}require(rejected,"desktop_udp_replay_accepted");
    Cipher other(proof(root,server,client,"ALVR-DESKTOP-control-s2c-v1"));rejected=false;try{other.open(record);}catch(...){rejected=true;}require(rejected,"desktop_cipher_direction_accepted");
    const auto input_packet=input({EventKind::Button,0,.4f,.6f,1,1});Reader r(input_packet,true);require(r.integer(4)==11,"desktop_input_enum");auto e=event(r.blob());r.end();require(e.kind==EventKind::Button&&e.a==1&&e.b==1,"desktop_input_button_roundtrip");
    Bytes payload(4000,17);const auto pieces=surfxr::transport::shards(3,0,payload,packet_size,true);surfxr::transport::Reassembler reassembler(packet_size,true);std::optional<surfxr::transport::Packet> picture;
    for(const auto& shard:pieces)if(auto result=reassembler.accept(shard,1))picture=std::move(result);require(picture&&picture->bytes==payload,"desktop_media_reassembly");
    std::cout<<"SURFXR desktop offline: encryption, direction separation, tamper/replay rejection, input edges, Dashboard revocation and production SURFXR reassembly passed. Isolated loopback only; no capture, input injection or services.\n";return 0;
}
}
int main(int argc,char** argv){
    try{
        Options o;std::string generate,color_output;bool gpu_test=false,list_monitors=false;unsigned ime_seconds=0;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];
            if(arg=="--ime-probe-synthetic")return ime_capture_synthetic_check();
            if(arg=="--ime-probe-window"){require(i+1<argc,"ime_probe_parent_required");return ime_probe_window(number(argv[++i],1,0x7fffffff));}
            if(arg=="--host")continue;
            if(arg=="--dashboard-managed"){o.dashboard_managed=true;continue;}
            if(arg=="--gpu-self-test"){gpu_test=true;continue;}
            if(arg=="--color-output"){require(i+1<argc&&color_output.empty(),"desktop_color_output_argument");color_output=argv[++i];require(!color_output.empty(),"desktop_color_output_argument");continue;}
            if(arg=="--list-monitors"){list_monitors=true;continue;}
            if(arg=="--self-test"){o.self_test=true;continue;}if(arg=="--service"){o.service=true;continue;}if(arg=="--agent"){o.agent=true;continue;}if(arg=="--install-service"){o.install=true;continue;}if(arg=="--uninstall-service"){o.uninstall=true;continue;}
            if(arg=="--help"){std::cout<<"surfxr-desktop --host --dashboard-managed [--bind IPv4] [--port 19950]\nDashboard supplies trusted single-use sessions, permissions, FPS and bitrate. --install-service --dashboard-managed installs the optional SYSTEM host (Administrator); --uninstall-service removes only the service. --self-test is offline. --gpu-self-test checks synthetic GPU encoding; optional --color-output DIRECTORY saves its synthetic HEVC fixture. --ime-probe SECONDS (1..120) reports live candidate capture status/discovery/size without saving pixels or injecting input. --ime-probe-synthetic briefly shows its own cross-process popup and verifies capture/hit testing; no input injection or saved media. Legacy standalone mode: --host --key-file PATH [--fps 60] [--bitrate 40] [--monitor 0]; --generate-key PATH writes its private pairing key.\n";return 0;}
            require(i+1<argc,"desktop_missing_option_value");const char* value=argv[++i];
            if(arg=="--ime-probe"){ime_seconds=number(value,1,120);continue;}
            if(arg=="--key-file")o.key_file=value;else if(arg=="--stop-event")o.stop_event=value;else if(arg=="--bind")o.bind=value;else if(arg=="--port")o.port=number(value,1024,65533);else if(arg=="--fps")o.fps=number(value,30,144);else if(arg=="--bitrate")o.bitrate=number(value,1,200);else if(arg=="--monitor")o.monitor=number(value,0,15);else if(arg=="--generate-key")generate=value;else fail("desktop_unknown_option");
        }
        require(color_output.empty()||(gpu_test&&!o.self_test&&!o.uninstall&&!o.install&&!o.service&&!o.agent&&!list_monitors&&!ime_seconds&&generate.empty()),"desktop_color_output_requires_gpu_self_test");
        if(o.self_test)return self_test();if(o.uninstall)return uninstall_service();
        if(ime_seconds)return ime_probe(ime_seconds);
        if(list_monitors){
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            std::cout<<"[";bool first=true;for(auto monitor:capture_monitors(o.monitor)){Capture capture(monitor,true);const auto& d=capture.display();
                if(!first)std::cout<<",";first=false;std::cout<<"{\"monitor\":"<<monitor<<",\"x\":"<<d.x<<",\"y\":"<<d.y<<",\"width\":"<<d.width<<",\"height\":"<<d.height<<"}";
            }std::cout<<"]\n";return 0;
        }
        if(gpu_test){
            std::vector<std::unique_ptr<Capture>> captures;std::vector<std::unique_ptr<Encoder>> encoders;
            for(auto monitor:capture_monitors(o.monitor)){captures.push_back(std::make_unique<Capture>(monitor,true));encoders.push_back(std::make_unique<Encoder>(*captures.back(),o.fps,o.bitrate));}
            for(unsigned id=0;id<captures.size();++id){rotation_self_test(captures[id]->device(),captures[id]->context());require(!encoders[id]->configuration().empty(),"desktop_gpu_configuration");for(unsigned i=0;i<3;++i){auto encoded=encoders[id]->encode(*captures[id],i==0,i+1);require(!encoded.bytes.empty()&&(i!=0||encoded.idr),"desktop_gpu_encode_failed");}}
            const auto checked_outputs=captures.size();encoders.clear();captures.clear();desktop_color_self_test(o.monitor,color_output);
            std::cout<<"D3D11 synthetic textures -> HEVC NVENC: "<<checked_outputs<<" simultaneous outputs, 3 frames each and decoder configurations passed. No desktop pixels, network or input injection.\n";return 0;
        }
        if(!generate.empty()){
            require(!std::filesystem::exists(generate),"desktop_key_already_exists");PSECURITY_DESCRIPTOR security=nullptr;require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)",SDDL_REVISION_1,&security,nullptr),"desktop_key_acl_failed");SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};
            Handle file(CreateFileW(std::filesystem::path(generate).c_str(),GENERIC_WRITE,0,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));LocalFree(security);require(file.value!=INVALID_HANDLE_VALUE,"desktop_key_create_failed");
            const auto key=random();constexpr char hex[]="0123456789abcdef";std::string encoded;for(auto b:key){encoded+=hex[b>>4];encoded+=hex[b&15];}encoded+='\n';DWORD written=0;require(WriteFile(file.value,encoded.data(),static_cast<DWORD>(encoded.size()),&written,nullptr)&&written==encoded.size(),"desktop_key_write_failed");std::cout<<"Pairing key written. Transfer it privately to the Frame's surfxr-desktop.key with mode 0600.\n";return 0;
        }
        if(o.service && o.port == 9950) {
            o.port=surfxr::ports::migrate_service_desktop(o.port);
            std::cerr<<"SurfXR service: migrated legacy desktop port to "<<o.port<<'\n';
        }
        require(o.dashboard_managed||!o.key_file.empty(),"desktop_key_file_required");if(o.install)return install_service(o);if(o.service)return service_main(o);
        SetConsoleCtrlHandler(interrupt,TRUE);
        Handle event;std::thread watcher;std::atomic_bool done{false};
        struct Join {std::atomic_bool& done;std::thread& t;~Join(){done.store(true);if(t.joinable())t.join();}} join{done,watcher};
        if(!o.stop_event.empty()){
            require(o.agent&&o.stop_event.rfind("Global\\SurfXRDesktopStop-",0)==0,"desktop_stop_event_invalid");event.value=OpenEventW(SYNCHRONIZE,FALSE,std::filesystem::path(o.stop_event).c_str());require(event.value,"desktop_stop_event_unavailable");
            watcher=std::thread([&]{while(!done.load())if(WaitForSingleObject(event.value,50)==WAIT_OBJECT_0){stopping.store(true);break;}});
        }
        return run_host(o,stopping);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
}
int main(int argc,char** argv){return surfxr::transport::desktop::main(argc,argv);}
