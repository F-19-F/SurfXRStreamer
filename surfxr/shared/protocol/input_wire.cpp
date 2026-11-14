#include "surfxr/input_wire.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace surfxr {
namespace {
constexpr std::size_t kQueueLimit=256;
enum class Kind {Move,Button,Scroll,Key,Neutral,Unicode,Candidate};
struct Intent {Kind kind=Kind::Neutral;std::uint32_t stream=0;float u=0,v=0;int a=0,b=0;std::uint64_t candidate=0;};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
bool valid_scan(std::uint16_t value){
    return (value>=1 && value<=0x58 && value!=0x54 && value!=0x55)
        || value==0x70 || value==0x73 || value==0x79 || value==0x7b || value==0x7d
        || value==0xe01c || value==0xe01d || value==0xe035 || value==0xe038
        || value==0xe020 || value==0xe021 || value==0xe02e || value==0xe030 || value==0xe037 || value==0xe045
        || value==0xe047 || value==0xe048 || value==0xe049 || value==0xe04b
        || value==0xe04d || value==0xe04f || value==0xe050 || value==0xe051
        || value==0xe052 || value==0xe053 || value==0xe05b || value==0xe05c || value==0xe05d;
}
bool same_layout(const std::vector<InputMonitorRect>& a,const std::vector<InputMonitorRect>& b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(a[i].stream_id!=b[i].stream_id || a[i].x!=b[i].x || a[i].y!=b[i].y
       || a[i].width!=b[i].width || a[i].height!=b[i].height)return false;
    return true;
}
}
struct SessionInputQueue::Impl {
    mutable std::mutex mutex;
    std::deque<Intent> intents;
    std::vector<InputMonitorRect> monitors;
    std::array<bool,3> buttons{};
    InputStats stats;
    bool neutral_pending=false,last_right=false;
    double wheel_x=0,wheel_y=0;
    float desktop_u=0,desktop_v=0;unsigned desktop_stream=0;bool desktop_position=false;
    bool reject(bool release=false){++stats.rejected;if(release){intents.clear();neutral_pending=true;}return false;}
    bool push(Intent intent){
        if(intent.kind==Kind::Move && !intents.empty() && intents.back().kind==Kind::Move){
            intents.back()=intent;++stats.coalesced_moves;++stats.accepted;return true;
        }
        if(intents.size()>=kQueueLimit){++stats.queue_overflows;return reject(true);}
        intents.push_back(intent);++stats.accepted;return true;
    }
    void neutral(){buttons.fill(false);wheel_x=wheel_y=0;}
    void count_button_edges(){
        if(buttons[1]!=last_right){
            if(buttons[1])++stats.right_button_down_snapshots;
            else ++stats.right_button_up_snapshots;
            last_right=buttons[1];
        }
    }

};
SessionInputQueue::SessionInputQueue():impl_(new Impl){}
SessionInputQueue::~SessionInputQueue()=default;
void SessionInputQueue::use_desktop_events(){auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);s.intents.clear();s.neutral_pending=true;}
bool SessionInputQueue::next_desktop_event(transport::desktop::Event& event){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);Intent intent;
    if(s.neutral_pending){s.neutral_pending=false;intent.kind=Kind::Neutral;}
    else if(!s.intents.empty()){intent=s.intents.front();s.intents.pop_front();}
    else return false;
    event={};event.stream=intent.stream;event.u=intent.u;event.v=intent.v;
    event.a=intent.a;event.b=intent.b;event.candidate=intent.candidate;
    switch(intent.kind){
    case Kind::Neutral:event.kind=transport::desktop::EventKind::Release;s.neutral();s.desktop_position=false;++s.stats.release_snapshots;break;
    case Kind::Move:event.kind=transport::desktop::EventKind::Move;s.desktop_u=intent.u;s.desktop_v=intent.v;s.desktop_stream=intent.stream;s.desktop_position=true;break;
    case Kind::Button:
        if(intent.b&&!s.desktop_position)return s.reject(true);
        event.kind=transport::desktop::EventKind::Button;event.u=s.desktop_u;event.v=s.desktop_v;event.stream=s.desktop_stream;
        s.buttons[static_cast<unsigned>(intent.a)]=intent.b!=0;s.count_button_edges();break;
    case Kind::Scroll:event.kind=transport::desktop::EventKind::Wheel;event.a*=8;event.b*=8;break;
    case Kind::Key:event.kind=transport::desktop::EventKind::Key;break;
    case Kind::Unicode:event.kind=transport::desktop::EventKind::Unicode;break;
    case Kind::Candidate:event.kind=transport::desktop::EventKind::Button;break;
    }
    ++s.stats.snapshots;return true;
}
bool SessionInputQueue::candidate_click(std::uint64_t token,float u,float v){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!token||!(u>=0&&u<=1&&v>=0&&v<=1))return s.reject();
    return s.push({Kind::Candidate,0,u,v,0,1,token});
}
bool SessionInputQueue::pointer_move(std::uint32_t stream,float u,float v){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(stream>=4 || !std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1)return s.reject();
    return s.push({Kind::Move,stream,u,v,0,0});
}
bool SessionInputQueue::pointer_button(unsigned button,bool pressed){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(button>2)return s.reject(true);
    return s.push({Kind::Button,0,0,0,static_cast<int>(button),pressed?1:0});
}
bool SessionInputQueue::scroll(int horizontal,int vertical){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(horizontal<-127 || horizontal>127 || vertical<-127 || vertical>127)return s.reject();
    return s.push({Kind::Scroll,0,0,0,horizontal,vertical});
}
bool SessionInputQueue::scroll_wheel(float horizontal,float vertical){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!std::isfinite(horizontal)||!std::isfinite(vertical)||std::abs(horizontal)>1016||std::abs(vertical)>1016)return s.reject();
    s.wheel_x+=horizontal/8.0;s.wheel_y+=vertical/8.0;
    const int x=std::clamp(static_cast<int>(s.wheel_x),-127,127),y=std::clamp(static_cast<int>(s.wheel_y),-127,127);
    s.wheel_x-=x;s.wheel_y-=y;
    if(!x&&!y)return true;
    return s.push({Kind::Scroll,0,0,0,x,y});
}
bool SessionInputQueue::shortcut(const std::vector<ScanCode>& keys){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(keys.empty()||keys.size()>8)return s.reject(true);
    for(unsigned i=0;i<keys.size();++i)if(!valid_scan(static_cast<std::uint16_t>(keys[i]))||std::find(keys.begin(),keys.begin()+i,keys[i])!=keys.begin()+i)return s.reject(true);
    if(s.intents.size()+keys.size()*2+2>kQueueLimit){++s.stats.queue_overflows;return s.reject(true);}
    s.push({Kind::Neutral});
    for(auto code:keys)s.push({Kind::Key,0,0,0,static_cast<int>(code),1});
    for(auto it=keys.rbegin();it!=keys.rend();++it)s.push({Kind::Key,0,0,0,static_cast<int>(*it),0});
    s.push({Kind::Neutral});return true;
}
bool SessionInputQueue::key(ScanCode scan,bool pressed){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);const auto code=static_cast<std::uint16_t>(scan);
    if(!valid_scan(code))return s.reject(true);
    return s.push({Kind::Key,0,0,0,code,pressed?1:0});
}
bool SessionInputQueue::text_ascii(const std::string& text){
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    {
        if(text.empty()||text.size()>384)return s.reject();
        std::vector<Intent> events{{Kind::Neutral}};
        const auto unit=[&](unsigned u){events.push_back({Kind::Unicode,0,0,0,static_cast<int>(u),1});events.push_back({Kind::Unicode,0,0,0,static_cast<int>(u),0});};
        for(std::size_t at=0;at<text.size();){
            const auto lead=static_cast<unsigned char>(text[at++]);unsigned count=0,cp=lead,minimum=0;
            if(lead<128){}else if(lead>=0xc2&&lead<=0xdf){count=1;cp=lead&31;minimum=0x80;}else if(lead>=0xe0&&lead<=0xef){count=2;cp=lead&15;minimum=0x800;}else if(lead>=0xf0&&lead<=0xf4){count=3;cp=lead&7;minimum=0x10000;}else return s.reject();
            if(at+count>text.size())return s.reject();
            for(unsigned i=0;i<count;++i){const auto c=static_cast<unsigned char>(text[at++]);if((c&0xc0)!=0x80)return s.reject();cp=(cp<<6)|(c&63);}
            if(cp<minimum||cp<32||cp==127||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return s.reject();
            if(cp<0x10000)unit(cp);else{cp-=0x10000;unit(0xd800+(cp>>10));unit(0xdc00+(cp&1023));}
        }
        events.push_back({Kind::Neutral});if(events.size()+s.intents.size()>kQueueLimit){++s.stats.queue_overflows;return s.reject(true);}
        s.intents.insert(s.intents.end(),events.begin(),events.end());++s.stats.accepted;return true;
    }

}
void SessionInputQueue::release_all() noexcept{
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);s.intents.clear();s.neutral_pending=true;s.wheel_x=s.wheel_y=0;
}
InputStats SessionInputQueue::statistics() const{auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);return s.stats;}
void SessionInputQueue::set_monitors(const std::vector<InputMonitorRect>& monitors){
    require(!monitors.empty() && monitors.size()<=4,"invalid_input_monitor_count");
    std::array<bool,4> seen{};
    for(const auto& m:monitors){
        require(m.stream_id<4 && !seen[m.stream_id] && m.width>0 && m.height>0 && m.width<=32768 && m.height<=32768
                && m.x>=-32768 && m.y>=-32768 && std::int64_t{m.x}+m.width<=65536 && std::int64_t{m.y}+m.height<=65536,
                "invalid_input_monitor_rectangle");seen[m.stream_id]=true;
    }
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!same_layout(s.monitors,monitors)){s.monitors=monitors;s.intents.clear();s.neutral_pending=true;}
}
int input_wire_self_test(){
    {
        SessionInputQueue q;transport::desktop::Event e;
        require(q.shortcut({ScanCode::LeftWindows,ScanCode::Space}),"shortcut_admission");
        require(q.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release,"shortcut_initial_release");
        for(const auto& edge:std::array<std::pair<int,int>,4>{{{0xe05b,1},{0x39,1},{0x39,0},{0xe05b,0}}})
            require(q.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Key&&e.a==edge.first&&e.b==edge.second,"shortcut_order");
        require(q.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release&&!q.next_desktop_event(e),"shortcut_final_release");
        require(!q.shortcut({ScanCode::LeftShift,ScanCode::LeftShift}),"shortcut_duplicate_rejected");q.release_all();q.next_desktop_event(e);
        for(unsigned i=0;i<253;++i)require(q.key(ScanCode::Space,true),"fill_shortcut_queue");
        require(!q.shortcut({ScanCode::LeftWindows,ScanCode::Space}),"shortcut_overflow_atomic_reject");
        require(q.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release&&!q.next_desktop_event(e),"shortcut_overflow_all_up");
    }

    {
        SessionInputQueue events;events.use_desktop_events();transport::desktop::Event e;
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release,"desktop_events_initial_release");
        require(events.pointer_move(0,.25f,.5f)&&events.pointer_button(0,true)&&events.pointer_button(0,false),"desktop_events_click_admitted");
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Move,"desktop_events_pointer_first");
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Button&&e.b==1&&e.u==.25f,"desktop_events_press_location");
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Button&&e.b==0,"desktop_events_release_without_snapshot_delay");
        require(events.text_ascii("\xe4\xb8\xad\xf0\x9f\x98\x80")&&!events.text_ascii("\xc0\xaf"),"desktop_events_utf8_validation");
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release,"desktop_text_releases_modifiers");
        for(unsigned code:{0x4e2du,0xd83du,0xde00u})for(int down:{1,0})require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Unicode&&e.a==static_cast<int>(code)&&e.b==down,"desktop_text_unicode_edges");
        events.release_all();require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release&&!events.next_desktop_event(e),"desktop_events_final_release");
        require(events.candidate_click(42,.3f,.6f)&&events.next_desktop_event(e)&&e.candidate==42&&e.u==.3f&&e.v==.6f&&e.kind==transport::desktop::EventKind::Button,"candidate_uses_existing_input_queue");
        require(!events.candidate_click(0,.3f,.6f)&&!events.candidate_click(42,-1,.6f),"candidate_invalid_click_rejected");
        require(events.candidate_click(43,.3f,.6f),"candidate_queue_before_release");events.release_all();
        require(events.next_desktop_event(e)&&e.kind==transport::desktop::EventKind::Release&&!events.next_desktop_event(e),"candidate_mode_change_discards_old_click");
    }
    return 0;
}
} // namespace surfxr
