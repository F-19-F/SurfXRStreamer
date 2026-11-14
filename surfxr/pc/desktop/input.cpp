#include "input.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr::transport::desktop {
void Input::click(Display region,float u,float v){
    // Map the existing button path to a validated ROI; keep modifiers intact.
    const auto prior=display_;auto displays=displays_;const auto old_u=u_,old_v=v_;const bool positioned=position_;
    displays_={region};
    struct Restore {Input& self;Display display;std::vector<Display>& displays;float u,v;bool positioned;
        ~Restore(){self.display_=display;self.displays_=std::move(displays);self.u_=u;self.v_=v;self.position_=positioned;}
    }restore{*this,prior,displays,old_u,old_v,positioned};
    apply({EventKind::Button,0,u,v,0,1});apply({EventKind::Button,0,u,v,0,0});
}
bool Input::send(INPUT* items,unsigned count){
    for(unsigned n=0;n<count;++n)if(items[n].type==INPUT_MOUSE)items[n].mi.dwExtraInfo=MouseActivity::injection_tag;
    SetLastError(ERROR_SUCCESS);const auto sent=SendInput(count,items,sizeof(INPUT));const auto error=GetLastError();
    const bool accepted=sent==count;const auto now=std::chrono::steady_clock::now();
    if(now>=next_report_&&(!accepted||!injection_ready_)){
        report_event(accepted?"desktop_input_ready":"desktop_input_injection_denied",error,accepted?EVENTLOG_INFORMATION_TYPE:EVENTLOG_WARNING_TYPE);
        next_report_=now+std::chrono::seconds(5);
    }
    injection_ready_=accepted;return accepted;
}
INPUT Input::move()const{
    INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN),width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    require(width>0&&height>0,"desktop_input_virtual_screen_unavailable");
    // Address the center of the exact physical pixel. Negative monitor origins
    const double x=display_.x+std::round(u_*(display_.width-1)),y=display_.y+std::round(v_*(display_.height-1));
    i.mi.dx=static_cast<LONG>(std::clamp((x-left+.5)*65536/width,0.,65535.));i.mi.dy=static_cast<LONG>(std::clamp((y-top+.5)*65536/height,0.,65535.));return i;
}
INPUT Input::button(unsigned b,bool down){INPUT i{};i.type=INPUT_MOUSE;constexpr DWORD flags[3][2]{{MOUSEEVENTF_LEFTUP,MOUSEEVENTF_LEFTDOWN},{MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_RIGHTDOWN},{MOUSEEVENTF_MIDDLEUP,MOUSEEVENTF_MIDDLEDOWN}};i.mi.dwFlags=flags[b][down?1:0];return i;}
INPUT Input::key(unsigned code,bool down){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wScan=static_cast<WORD>(code&255);i.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP)|((code>>8)?KEYEVENTF_EXTENDEDKEY:0);if(code==0xe045){i.ki.wScan=0;i.ki.wVk=VK_PAUSE;i.ki.dwFlags=down?0:KEYEVENTF_KEYUP;}return i;}
void Input::release()noexcept{
    for(unsigned b=0;b<3;++b)if(buttons_[b])pending_release_.push_back(button(b,false));for(auto k:keys_)pending_release_.push_back(key(k,false));
    for(auto u:unicode_){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wScan=static_cast<WORD>(u);i.ki.dwFlags=KEYEVENTF_UNICODE|KEYEVENTF_KEYUP;pending_release_.push_back(i);}
    buttons_={};keys_.clear();unicode_.clear();position_=false;arbiter_.reset();
    if(!pending_release_.empty()){const auto sent=SendInput(static_cast<UINT>(pending_release_.size()),pending_release_.data(),sizeof(INPUT));pending_release_.erase(pending_release_.begin(),pending_release_.begin()+sent);}
}
bool Input::tick(){
    if(desktop_.changed()){
        release();activity_.stop();try{desktop_.attach();if(arbitrate_)require(activity_.start(),"desktop_mouse_hook_failed");}catch(const std::runtime_error&){return false;}
    }
    if(arbitrate_&&!activity_.start())return false;
    // A desktop transition can temporarily deny SendInput. Retain only all-up
    // edges and retry after attaching; never replay a stale click on UAC.
    if(!pending_release_.empty())release();return pending_release_.empty();
}
void Input::apply(const Event& e){
    if(e.kind==EventKind::Release){release();return;}
    if(e.kind==EventKind::Move||e.kind==EventKind::Button){
        if(e.stream>=displays_.size()){release();return;}display_=displays_[e.stream];
    }
    if(!tick())return;INPUT items[3]{};unsigned count=1;
    switch(e.kind){
    case EventKind::Release:release();return;
    case EventKind::Move:
        u_=e.u;v_=e.v;position_=true;
        if(arbitrate_&&!arbiter_.sample(display_.x+double(u_)*(display_.width-1),display_.y+double(v_)*(display_.height-1),PointerArbiter::Clock::now()))return;
        items[0]=move();break;
    case EventKind::Button:
        u_=e.u;v_=e.v;position_=true;if(buttons_[e.a]==bool(e.b))return;
        if(!arbitrate_||arbiter_.allows()){
            items[0]=move();items[1]=button(static_cast<unsigned>(e.a),e.b!=0);count=2;
        }else{
            // Never relocate or press at the PC cursor. Always release a VR
            // button we already own, even after PC movement wins arbitration.
            if(e.b)return;
            items[0]=button(static_cast<unsigned>(e.a),false);
        }
        // Conservatively retain a requested down before sending a batch: a
        // short SendInput may have injected its edge. Only retry all-up.
        if(e.b)buttons_[e.a]=true;
        if(send(items,count))buttons_[e.a]=e.b!=0;else release();return;
    case EventKind::Wheel:
        if(!position_||(!e.a&&!e.b)||(arbitrate_&&!arbiter_.allows()))return;
        // PC may have moved the cursor since the last VR sample. An allowed
        // explicit scroll must target the VR panel, just like an allowed click.
        items[0]=move();items[1].type=items[2].type=INPUT_MOUSE;count=1;
        if(e.a){items[count].mi.dwFlags=MOUSEEVENTF_HWHEEL;items[count++].mi.mouseData=static_cast<DWORD>(e.a);}
        if(e.b){items[count].mi.dwFlags=MOUSEEVENTF_WHEEL;items[count++].mi.mouseData=static_cast<DWORD>(e.b);}
        if(!count)return;break;
    case EventKind::Key:
        if((keys_.count(static_cast<unsigned>(e.a))!=0)==bool(e.b))return;items[0]=key(static_cast<unsigned>(e.a),e.b!=0);
        if(e.b)keys_.insert(static_cast<unsigned>(e.a));
        if(send(items,1)){if(!e.b)keys_.erase(static_cast<unsigned>(e.a));}else release();return;
    case EventKind::Unicode:
        items[0].type=INPUT_KEYBOARD;items[0].ki.wScan=static_cast<WORD>(e.a);items[0].ki.dwFlags=KEYEVENTF_UNICODE|(e.b?0:KEYEVENTF_KEYUP);
        if(e.b)unicode_.insert(static_cast<unsigned>(e.a));
        if(send(items,1)){if(!e.b)unicode_.erase(static_cast<unsigned>(e.a));}else release();return;
    }
    if(!send(items,count))release();
}
}
