#include "backend.hpp"
#include "../desktop/diagnostics.hpp"
#include "../desktop/media_lease.hpp"
#include <cstring>
#include "surfxr/surfxr_desktop_layout.hpp"
namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
namespace {
bool adjacent_moves(const Bytes& first,const Bytes& second){
    const auto move=[](const Bytes& bytes,unsigned& type,unsigned& generation,unsigned& stream){
        try{Reader r(bytes,true);type=static_cast<unsigned>(r.integer(4));generation=0;
            if(type==input_type)generation=static_cast<unsigned>(r.integer(4));else if(type!=11)return false;
            const auto value=event(r.blob(64));r.end();stream=value.stream;return value.kind==EventKind::Move;
        }catch(...){return false;}
    };
    unsigned a=0,b=0,ga=0,gb=0,sa=0,sb=0;
    return move(first,a,ga,sa)&&move(second,b,gb,sb)&&a==b&&ga==gb&&sa==sb;
}
int snapshot(const Bytes& bytes){
    try{
        Reader r(bytes,true);const auto type=r.integer(4);
        // Repeated keepalive/IDR requests carry no edges or payload. Keep one
        // pending request instead of exhausting the queue during a long stall.
        if(type==1||type==2){r.end();return type==2?3:4;}
        if(type==11){const auto blob=r.blob(4096);r.end();Reader b(blob);
            if(b.integer(4)!=0x5644534b||b.integer(4)!=1)return -1;
            const auto hand=b.integer(4),active=b.integer(4);b.integer(8);
            return hand<2&&active<=1&&b.remaining()==(active?31*28:0)?static_cast<int>(hand):-1;
        }
        if(type==6){const auto count=r.integer(8);if(count>128)return -1;
            for(unsigned i=0;i<count;++i){r.integer(8);const auto kind=r.integer(4);if(kind==0)r.boolean();else if(kind==1)r.real();else return -1;}r.end();return 2;
        }
    }catch(...){}return -1;
}
bool same_button_edges(const Bytes& first,const Bytes& second){
    try{
        Reader a(first,true),b(second,true);a.integer(4);b.integer(4);const auto count=a.integer(8);if(count!=b.integer(8))return false;
        for(unsigned i=0;i<count;++i){if(a.integer(8)!=b.integer(8))return false;const auto kind=a.integer(4);if(kind!=b.integer(4))return false;
            if(kind==0){if(a.boolean()!=b.boolean())return false;}else{a.real();b.real();}}
        return true;
    }catch(...){}return false;
}
}
Backend::Backend(bool desktop,unsigned generation,Send control,VideoSend video):desktop_(desktop),generation_(generation),control_(std::move(control)),video_(std::move(video)),controls_ready_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),stop_ready_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){require(controls_ready_.value&&stop_ready_.value,"daemon_backend_event");}
Backend::~Backend(){stop();}
void Backend::stop(){stop_.store(true);SetEvent(stop_ready_.value);SetEvent(controls_ready_.value);{std::lock_guard<std::mutex> lock(mutex_);if(control_socket_!=INVALID_SOCKET)shutdown(control_socket_,SD_BOTH);}ready_.notify_all();if(thread_.joinable())thread_.join();}
bool Backend::push(Bytes bytes){
    constexpr std::size_t budget=4*1024*1024;
    if(bytes.size()>budget)return false;
    std::unique_lock<std::mutex> lock(mutex_);
    if(!desktop_){const auto key=snapshot(bytes);
        if(key>=0)for(auto it=controls_.rbegin();it!=controls_.rend();++it){const auto old=snapshot(*it);
            if(old<0||(key<2&&old==2))break;
            if(old==key){if(key==2&&!same_button_edges(*it,bytes))break;
                if(queued_bytes_-it->size()+bytes.size()>budget)return false;
                queued_bytes_-=it->size();queued_bytes_+=bytes.size();*it=std::move(bytes);return true;
            }
        }
    }
    // Merge only adjacent moves on the same layout/stream. Never pass a key,
    // button or all-up edge, and keep the queue bounded under a slow consumer.
    if(desktop_&&!controls_.empty()&&adjacent_moves(controls_.back(),bytes)){
        queued_bytes_-=controls_.back().size();queued_bytes_+=bytes.size();controls_.back()=std::move(bytes);return true;
    }
    if(!ready_.wait_for(lock,std::chrono::seconds(2),[&]{return stopped()||finished()||(controls_.size()<64&&queued_bytes_+bytes.size()<=budget);}))return false;
    // Late controls for an owner that already retired cannot close the outer
    // authenticated session. Ended still reports that owner's actual failure.
    if(stopped()||finished())return true;
    queued_bytes_+=bytes.size();controls_.push_back(std::move(bytes));SetEvent(controls_ready_.value);ready_.notify_all();return true;
}
bool Backend::readable(unsigned milliseconds){std::unique_lock<std::mutex> lock(mutex_);ready_.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{return stopped()||!controls_.empty();});return stopped()||!controls_.empty();}
int Backend::receive(std::uint8_t* bytes,std::size_t capacity){
    std::unique_lock<std::mutex> lock(mutex_);const auto deadline=Clock::now()+std::chrono::seconds(10);
    ready_.wait_until(lock,deadline,[&]{return stopped()||!controls_.empty();});
    if(stopped()||controls_.empty()||controls_.front().size()>capacity)return -1;
    auto value=std::move(controls_.front());controls_.pop_front();queued_bytes_-=value.size();if(controls_.empty()&&!stopped())ResetEvent(controls_ready_.value);ready_.notify_all();std::memcpy(bytes,value.data(),value.size());return static_cast<int>(value.size());
}
bool Backend::uplink(const std::uint8_t* bytes,std::size_t size){
    std::lock_guard<std::mutex> lock(mutex_);if(stopped()||uplink_==INVALID_SOCKET)return true;
    if(send(uplink_,reinterpret_cast<const char*>(bytes),static_cast<int>(size),0)==static_cast<int>(size))return true;
    const auto error=WSAGetLastError();return error==WSAECONNRESET||error==WSAECONNREFUSED||error==WSAENOBUFS||error==WSAEWOULDBLOCK;
}
void Backend::desktop(DesktopRun run,unsigned fps,unsigned bitrate,bool input,std::function<unsigned(unsigned)> monitors,std::function<bool()> idle,std::function<bool()> prefer_pc){
    thread_=std::thread([this,run,fps,bitrate,input,monitors=std::move(monitors),idle=std::move(idle),prefer_pc=std::move(prefer_pc)]{
        try{
            const auto lease_started=Clock::now();MediaLease lease;report_duration("daemon_desktop_lease_ms",lease_started);
            // The driver can time out its IPC shutdown. Keep a second barrier
            // here until the daemon's actual encoder/GPU/send owner has joined.
            const auto deadline=Clock::now()+std::chrono::seconds(8);
            while(!idle()&&!stopped()&&Clock::now()<deadline)Sleep(2);
            require(idle()&&!stopped(),"daemon_previous_encoder_still_active");
            struct Owner {Backend* backend;std::function<unsigned(unsigned)> monitors;std::function<bool()> prefer_pc;} owner{this,monitors,prefer_pc};
            DesktopSession api;api.owner=&owner;api.fps=fps;api.bitrate=bitrate;api.input=input;
            api.stopped=[](void* p){return static_cast<Owner*>(p)->backend->stopped();};
            api.readable=[](void* p,unsigned ms){return static_cast<Owner*>(p)->backend->readable(ms);};
            api.receive=[](void* p,std::uint8_t* bytes,std::size_t size){return static_cast<Owner*>(p)->backend->receive(bytes,size);};
            api.control=[](void* p,const std::uint8_t* bytes,std::size_t size){return static_cast<Owner*>(p)->backend->control_(Bytes(bytes,bytes+size));};
            api.video=[](void* p,const std::uint8_t* bytes,std::size_t size){auto* backend=static_cast<Owner*>(p)->backend;return !backend->stopped()&&backend->video_(bytes,size);};
            api.monitors=[](void* p,unsigned count){return static_cast<Owner*>(p)->monitors(count);};
            api.prefer_pc_mouse=[](void* p){const auto& f=static_cast<Owner*>(p)->prefer_pc;return f&&f();};
            run(&api);
        }catch(const std::exception& error){if(!stopped())report_failure(error,"daemon_desktop_backend_failed");}
        finished_.store(true);ready_.notify_all();
    });
}
void Backend::vr(Bytes caps,Worker& worker){thread_=std::thread([this,caps=std::move(caps),&worker]{try{run_vr(caps,worker);}catch(const std::exception& error){if(!stopped())report_failure(error,"daemon_vr_backend_failed");}finished_.store(true);ready_.notify_all();});}
void Backend::tracking(Bytes caps){thread_=std::thread([this,caps=std::move(caps)]{try{run_tracking(caps);}catch(const std::exception& error){if(!stopped())report_failure(error,"daemon_tracking_backend_failed");}finished_.store(true);ready_.notify_all();});}
}
