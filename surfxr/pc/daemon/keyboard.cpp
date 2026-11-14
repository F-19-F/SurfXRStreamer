#include "keyboard.hpp"
#include "../desktop/ime_capture.hpp"
#include "../desktop/ime_events.hpp"
#include "../desktop/diagnostics.hpp"
#include "surfxr/keyboard_image.hpp"
namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
Keyboard::Keyboard(Publish update,bool clickable,bool candidates_only):update_(std::move(update)),clickable_(clickable),candidates_only_(candidates_only),thread_([this]{run();}){}
Keyboard::~Keyboard(){
    {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}ready_.notify_one();thread_.join();
    if(update_)update_(nullptr,0,false);
}
void Keyboard::visible(bool value){
    {std::lock_guard<std::mutex> lock(mutex_);if(visible_&&!value){pending_.clear();pending_.push_back({});}visible_=value;}
    ready_.notify_one();
}
bool Keyboard::input(const Bytes& bytes){
    const auto e=keyboard_input_event(bytes,candidates_only_);if(!e)return false;
    if(e->candidate&&!clickable_)return true;
    {std::lock_guard<std::mutex> lock(mutex_);
        if(pending_.size()>=128){pending_.clear();pending_.push_back({});return true;}
        pending_.push_back(*e);
    }ready_.notify_one();return true;
}
void Keyboard::run(){
    // The existing input owner attaches to the interactive desktop and retains
    // only all-up edges across access loss. Never replay a failed key press.
    try{
        Input input(Display{},false);ImeObserver observer;auto capture_at=std::chrono::steady_clock::now(),captured_at=capture_at;
        ImeCandidate candidate;std::uint64_t serial=0,token=0;unsigned last_capture_status=99;
        auto click_report_at=capture_at;
        const auto click_report=[&](unsigned reason){const auto now=std::chrono::steady_clock::now();
            if(now>=click_report_at){report_event("daemon_ime_click_check",reason,EVENTLOG_INFORMATION_TYPE);click_report_at=now+std::chrono::seconds(1);}};
        while(true){
            std::deque<Event> events;bool visible=false;
            {std::unique_lock<std::mutex> lock(mutex_);ready_.wait_for(lock,std::chrono::milliseconds(10),[&]{return stop_||!pending_.empty();});
                if(stop_)break;visible=visible_;events.swap(pending_);}
            const bool ready=input.tick();
            if(!visible||!ready){candidate={};token=0;}
            for(const auto& e:events){
                if(e.candidate){
                    if(!ready||!visible){click_report(1);continue;}
                    if(e.candidate!=token){click_report(2);continue;}
                    if(std::chrono::steady_clock::now()-captured_at>=std::chrono::milliseconds(500)){click_report(3);continue;}
                    // Use the displayed ROI directly. Screen pixels are not a
                    // content identity: games animate behind translucent IMEs.
                    if(candidate.contains(e.u,e.v)){
                        click_report(0);input.click(candidate,e.u,e.v);
                    }else click_report(6);
                    candidate={};token=0;capture_at=std::chrono::steady_clock::now();
                }else if(ready||e.kind==EventKind::Release)input.apply(e);
            }
            if(update_&&std::chrono::steady_clock::now()>=capture_at){
                unsigned status=0;ImeCandidate fresh;if(visible&&ready)fresh=capture_ime_candidate(&status,&observer);
                if(status!=last_capture_status){report_event("daemon_ime_capture_status",status,EVENTLOG_INFORMATION_TYPE);last_capture_status=status;}
                if(!candidate.same_target(fresh))token=fresh.image.empty()?0:++serial;
                candidate=std::move(fresh);captured_at=std::chrono::steady_clock::now();
                const auto image=clickable_?keyboard_image::identify(candidate.image,token):candidate.image;
                if(update_)update_(image.data(),image.size(),true);
                capture_at=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
            }
        }
    }catch(...){if(update_)update_(nullptr,0,false);}
}
}
