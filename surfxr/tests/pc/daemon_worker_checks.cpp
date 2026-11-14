#include "../../pc/daemon/worker.hpp"
#include "../../pc/desktop/network.hpp"
#include "platform/win32/frame-worker/FrameDaemonIPC.h"
#include "platform/win32/frame-worker/FrameWorkerShared.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <future>
void daemon_backend_checks();

namespace {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
using frame_worker::Shared;using frame_worker::DaemonRequest;using frame_worker::DaemonReply;
std::atomic_uint calls{0};
bool socket_case=false;
bool direct_expected=true,late_retirement=false;
std::atomic_uint borrowed{0};
bool stalled_case=false;
std::atomic_bool callback_entered{false};
HANDLE callback_release=nullptr,engine_release=nullptr;
std::weak_ptr<int> callback_owner;
// Substitute only encoding. Windows pipe admission, handle duplication,
// mapping, cancellation, ownership and teardown are production methods.
int engine(std::uint64_t mapping_value,std::uint64_t wake_value,std::uint64_t socket){
    require((socket!=0)==socket_case,"daemon_fixture_socket_ownership");
    Socket video(socket?static_cast<SOCKET>(socket):INVALID_SOCKET);
    if(socket_case)require(send(video.value,"ipc",3,0)==3,"daemon_fixture_socket_send");
    Handle mapping(reinterpret_cast<HANDLE>(mapping_value)),wake(reinterpret_cast<HANDLE>(wake_value));
    auto* shared=static_cast<Shared*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!shared)return 1;
    struct View {Shared* shared;~View(){UnmapViewOfFile(shared);}} view{shared};
    ++calls;InterlockedExchange(&shared->ready,1);
    while(!frame_worker::load(shared->stop))WaitForSingleObject(wake.value,2);
    for(auto& slot:shared->slots){CloseHandle(reinterpret_cast<HANDLE>(slot.textureHandle));slot.textureHandle=0;}
    InterlockedExchange(&shared->ready,3);return 0;
}
int direct_engine(std::uint64_t mapping,std::uint64_t wake,void* owner,surfxr::daemon::WorkerSend send){
    require(owner&&send,"daemon_fixture_direct_admission");
    bool callback_result=false;std::thread callback;
    if(stalled_case)callback=std::thread([&]{callback_result=send(owner,reinterpret_cast<const std::uint8_t*>("ipc"),3);});
    else callback_result=send(owner,reinterpret_cast<const std::uint8_t*>("ipc"),3);
    const auto result=engine(mapping,wake,0);
    if(callback.joinable())callback.join();
    require(callback_result==direct_expected,"daemon_fixture_direct_result");
    if(late_retirement)require(send(owner,reinterpret_cast<const std::uint8_t*>("late"),4),"daemon_fixture_retired_callback_not_harmless");
    if(stalled_case)require(WaitForSingleObject(engine_release,1000)==WAIT_OBJECT_0,"daemon_fixture_stalled_engine_release");
    return result;
}
Handle connect_pipe(const std::wstring& name){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<deadline){
        const auto pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE)return Handle(pipe);Sleep(2);
    }
    fail("daemon_test_pipe_timeout");
}
void wait(std::function<bool()> ready){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!ready()&&std::chrono::steady_clock::now()<deadline)Sleep(2);require(ready(),"daemon_test_deadline");}
void partial_control(){
    auto listener=Socket::bind("127.0.0.1",0,true);sockaddr_in address{};int length=sizeof(address);
    require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"daemon_fixture_control_port");
    Socket writer(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));require(connect(writer.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"daemon_fixture_control_connect");
    Control reader(Socket(accept(listener.value,nullptr,nullptr)));require(reader.socket.value!=INVALID_SOCKET,"daemon_fixture_control_accept");
    const Bytes prefix{4,0};writer.send_all(prefix);require(!reader.poll_receive(),"daemon_fixture_partial_prefix");
    Sleep(2200);const auto began=Clock::now();require(!reader.poll_receive()&&Clock::now()-began<std::chrono::milliseconds(250),"daemon_fixture_busy_source_not_disconnect");
    writer.send_all(Bytes{0,0,1,2,3,4,4,0,0,0,5,6,7,8});
    std::optional<Bytes> first;wait([&]{if(!first)first=reader.poll_receive();return bool(first);});require(*first==Bytes({1,2,3,4}),"daemon_fixture_partial_control_preserved");
    require(reader.poll_receive()==std::optional<Bytes>(Bytes{5,6,7,8}),"daemon_fixture_coalesced_control_preserved");
    shutdown(writer.value,SD_SEND);bool closed=false;
    wait([&]{try{reader.poll_receive();}catch(const std::exception& e){closed=std::strcmp(e.what(),"desktop_control_closed")==0;}return closed;});
}
void session(surfxr::daemon::Worker& worker,const std::wstring& name,bool disconnect,Socket* receiver=nullptr,bool retire=false){
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),nullptr)),wake(CreateEventW(nullptr,FALSE,FALSE,nullptr));require(mapping.value&&wake.value,"daemon_test_handles");
    auto* shared=static_cast<Shared*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));require(shared,"daemon_test_mapping");
    struct View {Shared* shared;~View(){UnmapViewOfFile(shared);}} view{shared};
    shared->magic=frame_worker::Magic;shared->version=frame_worker::Version;shared->bytes=sizeof(Shared);shared->width=shared->height=64;
    std::array<Handle,frame_worker::SlotCount> slots;
    for(unsigned i=0;i<slots.size();++i){slots[i].value=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(slots[i].value,"daemon_test_slot_handle");shared->slots[i].textureHandle=reinterpret_cast<std::uint64_t>(slots[i].value);}
    auto pipe=connect_pipe(name);DaemonRequest request;request.mapping=reinterpret_cast<std::uint64_t>(mapping.value);request.wake=reinterpret_cast<std::uint64_t>(wake.value);DWORD bytes=0;
    // Exercise a fragmented request instead of assuming one complete read.
    require(WriteFile(pipe.value,&request,8,&bytes,nullptr)&&bytes==8,"daemon_test_prefix");Sleep(2);
    require(WriteFile(pipe.value,reinterpret_cast<const BYTE*>(&request)+8,sizeof(request)-8,&bytes,nullptr)&&bytes==sizeof(request)-8,"daemon_test_request");
    DaemonReply reply{};require(ReadFile(pipe.value,&reply,sizeof(reply),&bytes,nullptr)&&bytes==sizeof(reply)&&reply.magic==request.magic&&reply.pid==GetCurrentProcessId(),"daemon_test_same_process");
    wait([&]{return frame_worker::load(shared->ready)==1;});require(!worker.idle(),"daemon_test_busy_encoder");
    if(receiver){sockaddr_in source{};int length=sizeof(source);char payload[8]{};require(receiver->ready(1000),"daemon_fixture_socket_receive");
        require(recvfrom(receiver->value,payload,8,0,reinterpret_cast<sockaddr*>(&source),&length)==3&&std::memcmp(payload,"ipc",3)==0,"daemon_fixture_socket_bytes");
        require(source.sin_addr.s_addr==htonl(0x7f000001)&&ntohs(source.sin_port)==worker.sender_port(),"daemon_fixture_published_sender");}
    const auto retired_at=std::chrono::steady_clock::now();
    if(stalled_case){
        wait([]{return callback_entered.load();});
        auto retiring=std::async(std::launch::async,[&]{worker.destination(0);});
        require(retiring.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"daemon_fixture_inflight_not_joined");
        SetEvent(callback_release);
        require(retiring.wait_for(std::chrono::milliseconds(250))==std::future_status::ready,"daemon_fixture_retire_waited_stalled_encoder");retiring.get();
        require(!worker.idle()&&callback_owner.expired(),"daemon_fixture_retire_kept_callback_owner");
        SetEvent(engine_release);
    }else if(retire)worker.destination(0); // Producer and pipe remain alive and silent.
    else if(disconnect){CloseHandle(pipe.value);pipe.value=nullptr;}
    else{InterlockedExchange(&shared->stop,1);SetEvent(wake.value);}
    wait([&]{return frame_worker::load(shared->ready)==3&&worker.idle();});
    if(retire)require(std::chrono::steady_clock::now()-retired_at<std::chrono::milliseconds(250),"daemon_fixture_retire_requires_no_producer_response");
    require(worker.sender_port()==0,"daemon_fixture_sender_not_retired");
}
}
int main(int argc,char** argv){try{
    require(argc==1||(argc==2&&std::string(argv[1])=="--socket-owner-check"),"daemon_fixture_arguments");socket_case=argc==2;
    daemon_backend_checks();
    WSADATA startup{};if(socket_case)require(WSAStartup(MAKEWORD(2,2),&startup)==0,"daemon_fixture_winsock");
    struct Cleanup {~Cleanup(){if(socket_case)WSACleanup();}}cleanup;
    if(socket_case)partial_control();
    auto receiver=socket_case?Socket::bind("127.0.0.2",0,false):Socket{};unsigned destination=0;
    if(socket_case){sockaddr_in local{};int length=sizeof(local);require(getsockname(receiver.value,reinterpret_cast<sockaddr*>(&local),&length)==0,"daemon_fixture_endpoint");destination=ntohs(local.sin_port);}
    wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)>0,"daemon_test_executable");
    const auto name=L"\\\\.\\pipe\\SteamFrameSURFXRDaemonFixture-"+std::to_wstring(GetCurrentProcessId());
    DWORD before=0,after=0;
    {
        surfxr::daemon::Worker worker(engine,name,std::filesystem::path(executable).filename().wstring());
        worker.destination(destination);session(worker,name,false,socket_case?&receiver:nullptr);session(worker,name,true,socket_case?&receiver:nullptr);
        worker.destination(destination);session(worker,name,false,socket_case?&receiver:nullptr,true);
    }
    // Direct production admission borrows shards synchronously and retains the
    // owner through cancellation/join; it needs no loopback endpoint.
    {
        const bool previous_socket_case=socket_case;socket_case=false;
        surfxr::daemon::Worker worker(direct_engine,name,std::filesystem::path(executable).filename().wstring());
        const auto sink=[](const std::uint8_t* bytes,std::size_t size){require(size==3&&std::memcmp(bytes,"ipc",3)==0,"daemon_fixture_borrowed_bytes");++borrowed;return true;};
        worker.destination(sink);session(worker,name,false);worker.destination(sink);
        late_retirement=true;session(worker,name,false,nullptr,true);late_retirement=false;
        direct_expected=false;worker.destination([](const std::uint8_t*,std::size_t){return false;});session(worker,name,true);
        worker.destination([](const std::uint8_t*,std::size_t)->bool{throw std::runtime_error("fixture_send_failure");});session(worker,name,false);
        direct_expected=true;socket_case=previous_socket_case;
        require(borrowed==2,"daemon_fixture_direct_callbacks_bounded");
    }
    {
        const bool previous_socket_case=socket_case;socket_case=false;stalled_case=late_retirement=true;
        Handle release(CreateEventW(nullptr,TRUE,FALSE,nullptr)),retired(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        require(release.value&&retired.value,"daemon_fixture_barrier_events");callback_release=release.value;engine_release=retired.value;
        surfxr::daemon::Worker worker(direct_engine,name,std::filesystem::path(executable).filename().wstring());
        auto owner=std::make_shared<int>(1);callback_owner=owner;
        worker.destination([owner](const std::uint8_t*,std::size_t){callback_entered.store(true);return WaitForSingleObject(callback_release,1000)==WAIT_OBJECT_0;});owner.reset();
        session(worker,name,false,nullptr,true);
        stalled_case=late_retirement=false;socket_case=previous_socket_case;
    }
    // Warm up Windows/CRT paths before measuring repeated session ownership.
    require(GetProcessHandleCount(GetCurrentProcess(),&before),"daemon_test_handle_count");
    {surfxr::daemon::Worker worker(engine,name,std::filesystem::path(executable).filename().wstring());worker.destination(destination);session(worker,name,false,socket_case?&receiver:nullptr);session(worker,name,true,socket_case?&receiver:nullptr);}
    require(GetProcessHandleCount(GetCurrentProcess(),&after),"daemon_test_handle_count");require(calls==10&&after==before,"daemon_test_no_handle_leak");
    {
        surfxr::daemon::Worker worker(engine,name,L"wrong-caller.exe");auto pipe=connect_pipe(name);DaemonRequest request;DWORD bytes=0;WriteFile(pipe.value,&request,sizeof(request),&bytes,nullptr);
        wait([&]{return !PeekNamedPipe(pipe.value,nullptr,0,nullptr,nullptr,nullptr);});require(calls==10,"daemon_test_wrong_owner_rejected");
    }
    std::cout<<"Daemon CPU IPC: repeated sessions, fragmented request, stop, disconnect, caller rejection and no handle leak passed. "<<(socket_case?"Private synthetic loopback confirms same-process socket transfer and sender retirement. No GPU, media, audio devices or services.":"No GPU, media, audio devices, network or services.")<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
