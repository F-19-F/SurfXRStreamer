#include "worker.hpp"
#include "../desktop/network.hpp"
#include "../desktop/media_lease.hpp"
#include "platform/win32/frame-worker/FrameDaemonIPC.h"
#include "platform/win32/frame-worker/FrameWorkerShared.h"
#include <sddl.h>
#include <wtsapi32.h>
#include <filesystem>

namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
namespace {
bool transfer(HANDLE process,std::uint64_t source,Handle& target){return source&&DuplicateHandle(process,reinterpret_cast<HANDLE>(source),GetCurrentProcess(),&target.value,0,FALSE,DUPLICATE_SAME_ACCESS);}
bool owner(HANDLE pipe,Handle& process,const std::wstring& image){
    ULONG pid=0;DWORD session=0;
    if(!GetNamedPipeClientProcessId(pipe,&pid)||!ProcessIdToSessionId(pid,&session)||session!=WTSGetActiveConsoleSessionId())return false;
    process.value=OpenProcess(PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);if(!process.value)return false;
    wchar_t path[32768]{};DWORD size=32768;
    return QueryFullProcessImageNameW(process.value,0,path,&size)&&std::filesystem::path(path).filename()==image;
}
// Keep OVERLAPPED and its event alive until the kernel relinquishes them.
struct Pending {
    HANDLE pipe;Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED operation{};
    explicit Pending(HANDLE value):pipe(value){require(event.value,"daemon_worker_pipe_event");operation.hEvent=event.value;}
    ~Pending(){CancelIoEx(pipe,&operation);DWORD bytes=0;GetOverlappedResult(pipe,&operation,&bytes,TRUE);}
    bool finish(HANDLE stop,DWORD timeout,DWORD& bytes){
        const HANDLE events[]{stop,event.value};
        return WaitForMultipleObjects(2,events,FALSE,timeout)==WAIT_OBJECT_0+1&&GetOverlappedResult(pipe,&operation,&bytes,FALSE);
    }
};
}
struct Worker::SendGate {
    std::mutex mutex;std::condition_variable ready;VideoSend target;unsigned active=0;bool retired=false;
    explicit SendGate(VideoSend send):target(std::move(send)){}
    void retire(){
        std::unique_lock<std::mutex> lock(mutex);retired=true;
        ready.wait(lock,[&]{return active==0;});target={};
    }
    static bool send(void* owner,const std::uint8_t* bytes,std::size_t size){
        auto& gate=*static_cast<SendGate*>(owner);
        {std::lock_guard<std::mutex> lock(gate.mutex);
            // Old-generation sends are harmless drops, preserving cancellation
            // semantics without reporting a network fault or calling Session.
            if(gate.retired)return true;if(!gate.target)return false;++gate.active;}
        // Retirement cannot release the callable while a synchronous send is
        // active. No owner/Session/transport lock is held across this call.
        bool result=false;try{result=gate.target(bytes,size);}catch(...){}
        {std::lock_guard<std::mutex> lock(gate.mutex);if(!--gate.active)gate.ready.notify_all();}
        return result;
    }
};
Worker::Worker(WorkerRun run,std::wstring pipe,std::wstring image):Worker(static_cast<WorkerRunV7>(nullptr),std::move(pipe),std::move(image)){
    thread_=std::thread(&Worker::loop,this,run,nullptr);
}
Worker::Worker(WorkerRunV7 run,std::wstring pipe,std::wstring image):
    stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),retire_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),
    pipe_(pipe.empty()?frame_worker::DaemonPipe:std::move(pipe)),image_(std::move(image)){
    require(stop_event_.value&&retire_event_.value,"daemon_worker_events");if(run)thread_=std::thread(&Worker::loop,this,nullptr,run);
}
Worker::~Worker(){stop_.store(true);SetEvent(stop_event_.value);destination(0);if(thread_.joinable())thread_.join();}
void Worker::destination(unsigned port){
    std::shared_ptr<SendGate> gate;
    {std::lock_guard<std::mutex> lock(destination_mutex_);direct_={};destination_.store(port);gate=gate_;
        if(!port){retirement_.fetch_add(1);SetEvent(retire_event_.value);}}
    if(gate)gate->retire();
}
void Worker::destination(VideoSend send){
    std::lock_guard<std::mutex> lock(destination_mutex_);require(bool(send),"daemon_worker_direct_target_unset");
    direct_=std::move(send);destination_.store(0);
}
bool Worker::wait_idle(unsigned milliseconds){
    std::unique_lock<std::mutex> lock(destination_mutex_);
    return idle_ready_.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{return !busy_.load();});
}
void Worker::loop(WorkerRun run,WorkerRunV7 direct_run){
    using namespace frame_worker;
    while(!stop_.load()){
        PSECURITY_DESCRIPTOR security=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",SDDL_REVISION_1,&security,nullptr))return;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};
        Handle pipe(CreateNamedPipeW(pipe_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,1000,&attributes));LocalFree(security);
        if(pipe.value==INVALID_HANDLE_VALUE)return;
        bool connected=false;
        {Pending pending(pipe.value);DWORD bytes=0;connected=ConnectNamedPipe(pipe.value,&pending.operation)!=FALSE;
            if(!connected){const auto error=GetLastError();connected=error==ERROR_PIPE_CONNECTED||(error==ERROR_IO_PENDING&&pending.finish(stop_event_.value,INFINITE,bytes));}}
        if(!connected)break;
        const auto epoch=retirement_.load();
        try{
            Handle process,mapping,wake;require(owner(pipe.value,process,image_),"daemon_worker_owner_rejected");
            DaemonRequest request{};DWORD bytes=0;std::size_t received=0;const auto deadline=GetTickCount64()+2000;
            while(received<sizeof(request)&&!stop_.load()&&GetTickCount64()<deadline){
                Pending pending(pipe.value);const auto success=ReadFile(pipe.value,reinterpret_cast<BYTE*>(&request)+received,static_cast<DWORD>(sizeof(request)-received),&bytes,&pending.operation);
                const auto now=GetTickCount64();const auto remaining=deadline>now?static_cast<DWORD>(deadline-now):0;
                require(success||(GetLastError()==ERROR_IO_PENDING&&pending.finish(stop_event_.value,remaining,bytes)),"daemon_worker_request_failed");
                require(bytes,"daemon_worker_request_closed");received+=bytes;
            }
            require(received==sizeof(request)&&request.magic==0x46445731&&request.bytes==sizeof(request),"daemon_worker_request_invalid");
            require(transfer(process.value,request.mapping,mapping)&&transfer(process.value,request.wake,wake),"daemon_worker_handles_failed");
            auto* shared=static_cast<Shared*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));require(shared,"daemon_worker_mapping_failed");
            struct View {Shared* shared;~View(){UnmapViewOfFile(shared);}} view{shared};
            require(shared->magic==Magic&&shared->version==Version&&shared->bytes==sizeof(Shared)&&!shared->fixture&&shared->width&&shared->width<=16384&&shared->height&&shared->height<=16384,"daemon_worker_mapping_invalid");
            std::array<Handle,SlotCount> textures;
            for(unsigned i=0;i<SlotCount;++i){require(transfer(process.value,shared->slots[i].textureHandle,textures[i]),"daemon_worker_texture_failed");shared->slots[i].textureHandle=reinterpret_cast<std::uint64_t>(textures[i].value);}
            std::shared_ptr<SendGate> gate;unsigned media_destination=0;bool has_target=false;
            {std::lock_guard<std::mutex> lock(destination_mutex_);require(epoch==retirement_.load(),"daemon_worker_source_retired");
                has_target=bool(direct_);gate=std::make_shared<SendGate>(direct_);gate_=gate;media_destination=destination_.load();require(pipe_!=DaemonPipe||has_target||media_destination,"daemon_worker_video_target_unset");
                require(!stop_.load(),"daemon_worker_stopped");ResetEvent(retire_event_.value);busy_.store(true);}
            struct Busy {Worker* worker;~Busy(){std::lock_guard<std::mutex> lock(worker->destination_mutex_);worker->busy_.store(false);worker->idle_ready_.notify_all();}} busy{this};
            struct Gate {std::shared_ptr<SendGate> gate;~Gate(){gate->retire();}} gate_owner{gate};
            // The actual encoder owns the lease and retires independently of SteamVR.
            const auto lease_name=pipe_==DaemonPipe?std::wstring(L"Global\\SURFXRFrameMedia-v2"):L"Local\\"+pipe_.substr(pipe_.find_last_of(L'\\')+1)+L"-MediaLease";
            MediaLease media_lease(lease_name.c_str(),8000,[&]{return stop_.load()||epoch!=retirement_.load();},retire_event_.value);
            require(epoch==retirement_.load()&&!stop_.load(),"daemon_worker_source_retired");
            DaemonReply reply;reply.pid=GetCurrentProcessId();
            {Pending pending(pipe.value);const auto success=WriteFile(pipe.value,&reply,sizeof(reply),&bytes,&pending.operation);
                require((success||(GetLastError()==ERROR_IO_PENDING&&pending.finish(stop_event_.value,2000,bytes)))&&bytes==sizeof(reply),"daemon_worker_reply_failed");}
            // No post-admission pipe payload is valid. This read also wakes on disconnect.
            Pending disconnected(pipe.value);BYTE invalid=0;DWORD ignored=0;
            const auto read=ReadFile(pipe.value,&invalid,1,&ignored,&disconnected.operation);
            if(read||GetLastError()!=ERROR_IO_PENDING)SetEvent(disconnected.event.value);
            Handle finished(CreateEventW(nullptr,TRUE,FALSE,nullptr));require(finished.value,"daemon_worker_finished_event");
            const auto raw_wake=wake.value;
            std::thread cancellation([&]{const HANDLE events[]{finished.value,stop_event_.value,retire_event_.value,process.value,disconnected.event.value};
                if(WaitForMultipleObjects(5,events,FALSE,INFINITE)!=WAIT_OBJECT_0){InterlockedExchange(&shared->stop,1);SetEvent(raw_wake);gate->retire();}});
            struct Join {HANDLE done;std::thread& thread;~Join(){SetEvent(done);thread.join();}} join{finished.value,cancellation};
            auto media=media_destination?Socket::bind("127.0.0.1",0,false):Socket{};
            if(const auto port=media_destination){
                const int buffer=256*1024;require(setsockopt(media.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))==0,"daemon_worker_media_buffer");
                sockaddr_in address{},local{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<u_short>(port));inet_pton(AF_INET,"127.0.0.2",&address.sin_addr);
                require(::connect(media.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"daemon_worker_media_destination");int length=sizeof(local);
                require(getsockname(media.value,reinterpret_cast<sockaddr*>(&local),&length)==0,"daemon_worker_media_source");sender_port_.store(ntohs(local.sin_port));
            }else require(pipe_!=DaemonPipe||has_target,"daemon_worker_video_target_unset");
            struct Sender {std::atomic_uint& port;~Sender(){port.store(0);}}sender{sender_port_};
            // The gate itself outlives all sender calls, but its Session
            // callable is released promptly after retirement/in-flight sends.
            const auto map_handle=reinterpret_cast<std::uint64_t>(mapping.value),wake_handle=reinterpret_cast<std::uint64_t>(wake.value);
            const auto raw_socket=media.value;media.value=INVALID_SOCKET;
            for(auto& texture:textures)texture.value=nullptr;
            mapping.value=nullptr;wake.value=nullptr;
            if(direct_run)direct_run(map_handle,wake_handle,gate.get(),SendGate::send);
            else run(map_handle,wake_handle,raw_socket==INVALID_SOCKET?0:static_cast<std::uint64_t>(raw_socket));
            for(auto& slot:shared->slots)if(slot.textureHandle){CloseHandle(reinterpret_cast<HANDLE>(slot.textureHandle));slot.textureHandle=0;}
        }catch(...){/* Invalid/private IPC requests never become network sessions. */}
        DisconnectNamedPipe(pipe.value);
    }
}
}
