"""Compile production desktop input/attachment against fake Win32 calls.

No capture, input injection, desktop switching, service or event-log writes.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[3] / "surfxr"
sys.path.insert(0, str(PROJECT.parent / 'scripts/transport'))
from source_fixture import production_source, include_directories
from source_fixture import method


CPP = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include "surfxr/surfxr_desktop.hpp"
#include <algorithm>
#include <chrono>
#include <atomic>
#include <future>
#include <cmath>
#include <climits>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
using namespace surfxr::transport;
void check(bool ok){if(!ok)throw std::runtime_error("desktop_input_check_failed");}
std::deque<UINT> results;std::vector<std::vector<INPUT>> calls;
DWORD attach_access=0;bool denied=false;unsigned active=1,attached=1;
HDESK fake_open(DWORD,BOOL,DWORD access){attach_access=access;return denied?nullptr:reinterpret_cast<HDESK>(static_cast<uintptr_t>(active));}
BOOL fake_close(HDESK){return TRUE;}
HDESK fake_current(DWORD){return reinterpret_cast<HDESK>(static_cast<uintptr_t>(attached));}
BOOL fake_attach(HDESK h){attached=static_cast<unsigned>(reinterpret_cast<uintptr_t>(h));return TRUE;}
UINT fake_send(UINT count,LPINPUT data,int size){
    check(count&&size==sizeof(INPUT));calls.emplace_back(data,data+count);
    UINT n=count;if(!results.empty()){n=results.front();results.pop_front();}
    SetLastError(n==count?ERROR_SUCCESS:ERROR_ACCESS_DENIED);return n;
}
int fake_metrics(int key){switch(key){case SM_XVIRTUALSCREEN:return -128;case SM_YVIRTUALSCREEN:return 0;case SM_CXVIRTUALSCREEN:return 256;case SM_CYVIRTUALSCREEN:return 128;default:return 0;}}
#define OpenInputDesktop fake_open
#define CloseDesktop fake_close
#define GetThreadDesktop fake_current
#define SetThreadDesktop fake_attach
BOOL fake_cursor(POINT* point){*point={0,0};return TRUE;}
#define GetCursorPos fake_cursor
HOOKPROC mouse_hook=nullptr;DWORD hook_thread=0;
HHOOK fake_hook(int kind,HOOKPROC callback,HINSTANCE,DWORD){check(kind==WH_MOUSE_LL);mouse_hook=callback;hook_thread=GetCurrentThreadId();return reinterpret_cast<HHOOK>(1);}
BOOL fake_unhook(HHOOK){mouse_hook=nullptr;return TRUE;}
struct FakeMouse {MSLLHOOKSTRUCT event;std::promise<LRESULT> result;};
LRESULT fake_dispatch(const MSG* message){
    check(message->message==WM_APP);auto& command=*reinterpret_cast<FakeMouse*>(message->lParam);
    command.result.set_value(mouse_hook(HC_ACTION,WM_MOUSEMOVE,reinterpret_cast<LPARAM>(&command.event)));return 0;
}
LRESULT simulate_mouse(MSLLHOOKSTRUCT event){
    FakeMouse command{event,{}};auto result=command.result.get_future();
    check(PostThreadMessageW(hook_thread,WM_APP,0,reinterpret_cast<LPARAM>(&command)));
    check(result.wait_for(std::chrono::seconds(1))==std::future_status::ready);return result.get();
}
LRESULT fake_next(HHOOK,int,WPARAM,LPARAM){return 0;}
#define SetWindowsHookExW fake_hook
#define UnhookWindowsHookEx fake_unhook
#define DispatchMessageW fake_dispatch
#define CallNextHookEx fake_next
#define SendInput fake_send
#define GetSystemMetrics fake_metrics
HWND foreground=reinterpret_cast<HWND>(3),candidate_window=reinterpret_cast<HWND>(4);
RECT candidate_rect{-64,32,0,64};bool candidate_visible=true,covered=false;POINT candidate_point{};
RECT app_rect{-128,0,128,128};HWND owner=nullptr,focus=reinterpret_cast<HWND>(8);
DWORD candidate_pid=123,candidate_thread=321;LONG_PTR candidate_style=WS_POPUP;
HWND fake_foreground(){return foreground;}
BOOL fake_visible(HWND){return candidate_visible;}
BOOL fake_rect(HWND window,LPRECT rect){*rect=window==foreground?app_rect:candidate_rect;return TRUE;}
HWND fake_hit(POINT point){candidate_point=point;return covered?foreground:candidate_window;}
BOOL fake_child(HWND,HWND){return FALSE;}
HWND fake_owner(HWND window,UINT){return window==candidate_window?owner:nullptr;}
DWORD fake_thread(HWND window,DWORD* pid){if(pid)*pid=candidate_pid;return window==candidate_window?candidate_thread:111;}
HWND capture=nullptr;
BOOL fake_gui(DWORD,GUITHREADINFO* info){info->hwndFocus=focus;info->hwndCapture=capture;return TRUE;}
LONG_PTR fake_style(HWND,int){return candidate_style;}
BOOL fake_iconic(HWND){return FALSE;}
#define GetForegroundWindow fake_foreground
#define IsWindowVisible fake_visible
#define GetWindowRect fake_rect
#define WindowFromPoint fake_hit
#define IsChild fake_child
#define GetWindow fake_owner
#define GetWindowThreadProcessId fake_thread
#define GetGUIThreadInfo fake_gui
#define GetWindowLongPtrW fake_style
#define IsIconic fake_iconic
RECT clip_rect{-128,0,128,128};bool fullscreen=false,fail_down=false;
struct MouseMessage {UINT message;WPARAM buttons;LPARAM point;};
std::vector<MouseMessage> messages;
BOOL fake_clip(RECT* rect){*rect=clip_rect;return TRUE;}
HMONITOR fake_monitor(HWND,DWORD){return reinterpret_cast<HMONITOR>(1);}
BOOL fake_monitor_info(HMONITOR,MONITORINFO* info){info->rcMonitor=app_rect;return fullscreen;}
BOOL fake_client(HWND window,POINT* point){check(window==candidate_window);point->x-=candidate_rect.left;point->y-=candidate_rect.top;return TRUE;}
LRESULT fake_message(HWND window,UINT message,WPARAM w,LPARAM l,UINT flags,UINT timeout,PDWORD_PTR){
    check(window==candidate_window&&timeout==50&&(flags&SMTO_ABORTIFHUNG));messages.push_back({message,w,l});
    return !(message==WM_LBUTTONDOWN&&fail_down);
}
DPI_AWARENESS_CONTEXT fake_dpi(DPI_AWARENESS_CONTEXT context){return context;}
#define GetClipCursor fake_clip
#define MonitorFromWindow fake_monitor
#define GetMonitorInfoW fake_monitor_info
#define ScreenToClient fake_client
#define SendMessageTimeoutW fake_message
#define SetThreadDpiAwarenessContext fake_dpi
IME_HEADER
IME_EVENTS_HEADER
IME_UIA_HEADER
namespace surfxr::transport::desktop {
ImeAction action=ImeAction::Unavailable;unsigned actions=0;
ImeAction ime_uia_invoke(const ImeCandidate&,float,float){++actions;return action;}
}
namespace surfxr::transport::desktop {
IME_EVENT_METHODS
IME_REGION_SOURCE
IME_CONTAINS
DESKTOP_CLASS;
std::wstring name(HDESK h){return reinterpret_cast<uintptr_t>(h)==1?L"Default":L"Winlogon";}
ATTACH_METHOD
CHANGED_METHOD
void report_event(const char*,DWORD,WORD){}
}
ARBITER_HEADER
ACTIVITY_HEADER
ACTIVITY_SOURCE
INPUT_HEADER
INPUT_SOURCE
IME_CLICK_SOURCE
using namespace surfxr::transport::desktop;
int main(){try{
    using namespace std::chrono_literals;
    PointerArbiter arbitration;const auto t=PointerArbiter::Clock::time_point{};
    check(arbitration.sample(0,0,t));
    check(!arbitration.sample(.5,.5,t+10ms)); // tracking noise never keeps VR active
    check(arbitration.pc_moved(12,0,t+200ms));
    check(!arbitration.sample(.9,.9,t+201ms)&&!arbitration.allows());
    check(!arbitration.sample(3,0,t+400ms)); // timeout never admits small VR drift
    for(int i=0;i<1000;++i)check(!arbitration.sample(i%2?3:4,0,t+410ms+i*10ms));
    check(!arbitration.allows()); // bounded jitter cannot accumulate into takeover
    check(arbitration.sample(18,0,t+10420ms)); // deliberate VR motion takes over
    check(!arbitration.pc_moved(12,0,t+10421ms)); // simultaneous: VR wins by default
    arbitration.prefer_pc(true);
    check(arbitration.allows()); // policy edits alone never move the cursor
    check(arbitration.pc_moved(12,0,t+10422ms));
    check(!arbitration.sample(40,0,t+10423ms)); // simultaneous: PC wins
    check(!arbitration.sample(40,0,t+11000ms)); // denied motion is never replayed later
    check(!arbitration.sample(42,0,t+11001ms));
    check(!arbitration.sample(42,0,t+12000ms)); // partial intent expires even with idle samples
    check(!arbitration.sample(52,0,t+12001ms));
    check(arbitration.sample(54,0,t+12002ms)); // accumulate net continuous motion to 12 px
    check(arbitration.allows()); // both idle: last owner still owns
    check(!arbitration.pc_moved(1,0,t+13000ms));
    check(!arbitration.pc_moved(-1,0,t+13001ms));
    check(!arbitration.pc_moved(1,0,t+13002ms)); // PC jitter does not steal VR ownership
    check(!arbitration.pc_moved(6,0,t+14000ms));
    check(!arbitration.pc_moved(6,0,t+15000ms)); // separate small bursts do not accumulate
    check(arbitration.pc_moved(6,0,t+15001ms));
    arbitration.reset();check(!arbitration.allows());
    check(!arbitration.sample(1000,1000,t+16000ms)); // reacquiring ray/monitor establishes a baseline
    check(!arbitration.sample(1001,1001,t+17000ms));
    check(!arbitration.allows()); // reset and long inactivity preserve PC ownership
    check(arbitration.sample(1020,1000,t+17001ms));
    check(!arbitration.pc_moved(0,0,t+18000ms)); // duplicate native event never takes ownership
    // VR uses the original desktop envelope and the same production Input.
    const auto vr=keyboard_input_event(input({EventKind::Key,0,0,0,0x1e,1}));
    check(vr&&vr->kind==EventKind::Key&&vr->a==0x1e&&vr->b==1);
    for(const auto& ordinary:{Event{},Event{EventKind::Move},Event{EventKind::Button},Event{EventKind::Key,0,0,0,0x1e,1}})
        check(!keyboard_input_event(surfxr::transport::desktop::input(ordinary),true));
    const auto desktop_candidate=keyboard_input_event(surfxr::transport::desktop::input({EventKind::Button,0,.5f,.5f,0,1,42}),true);
    check(desktop_candidate&&desktop_candidate->candidate==42);
    Writer skeleton(true);skeleton.integer(11,4);skeleton.blob(Bytes(892,0x4b));
    check(!keyboard_input_event(skeleton.bytes)&&!keyboard_input_event(control(1)));
    Input input({-128,0,128,128,128,128});
    check((attach_access&GENERIC_WRITE)!=0&&attached==1);
    input.apply(*vr);check(calls.back()[0].ki.wScan==0x1e);
    input.apply(*keyboard_input_event(surfxr::transport::desktop::input({})));calls.clear();
    const auto click=keyboard_input_event(surfxr::transport::desktop::input({EventKind::Button,0,1,1,0,1,42}));
    check(click&&click->candidate==42&&click->u==1&&click->v==1);
    input.apply({EventKind::Key,0,0,0,0x2a,1});calls.clear();
    input.click({-64,32,64,32,0,0},click->u,click->v);
    check(calls.size()==2&&calls[0][0].mi.dx==32640&&calls[0][0].mi.dy==32512);
    check(calls[0][1].mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&calls[1][1].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    input.apply({EventKind::Key,0,0,0,0x2a,1});check(calls.size()==2); // click preserves held Shift
    input.release();calls.clear();
    ImeCandidate candidate{{1,2,3},candidate_window,foreground,candidate_rect};auto fresh=candidate;
    check(candidate.same_target(fresh)&&candidate.contains(1,1)&&candidate_point.x==-1&&candidate_point.y==63);
    fresh.image[0]=9;check(candidate.same_target(fresh)); // animation is not a new click target
    fresh.image.clear();check(!candidate.same_target(fresh));fresh=candidate;fresh.rect.left++;check(!candidate.same_target(fresh));
    fresh=candidate;fresh.foreground=nullptr;check(!candidate.same_target(fresh));
    fresh=candidate;fresh.process=999;check(!candidate.same_target(fresh));
    fresh=candidate;fresh.thread=999;check(!candidate.same_target(fresh));
    candidate.process=candidate_pid;candidate.thread=candidate_thread;
    candidate_pid++;check(!candidate.contains(.5f,.5f));candidate_pid--;
    candidate_thread++;check(!candidate.contains(.5f,.5f));candidate_thread--;
    covered=true;check(!candidate.contains(.5f,.5f));covered=false;
    candidate_visible=false;check(!candidate.contains(.5f,.5f));candidate_visible=true;
    candidate_rect.left++;check(!candidate.contains(.5f,.5f));candidate_rect=candidate.rect;
    foreground=nullptr;check(!candidate.contains(.5f,.5f));foreground=candidate.foreground;
    check(!candidate.contains(std::numeric_limits<float>::quiet_NaN(),.5f)&&!candidate.contains(-1,.5f));
    // A UIA candidate ROI can be smaller than its native text-input host.
    candidate.host_rect=candidate_rect;candidate.rect={-60,36,-4,60};
    check(candidate.contains(1,1)&&candidate_point.x==-5&&candidate_point.y==59);
    fresh=candidate;fresh.host_rect.right++;check(!candidate.same_target(fresh));
    candidate_rect.right++;check(!candidate.contains(.5f,.5f));candidate_rect=candidate.host_rect;
    // Ordinary apps retain SendInput; captured/clipped/fullscreen games must
    // never receive global mouse edges intended for the candidate host.
    calls.clear();input.click(candidate,1,1);check(calls.size()==2&&messages.empty()&&actions==0);calls.clear();
    capture=foreground;input.click(candidate,1,1);
    check(calls.empty()&&actions==1&&messages.size()==3);
    check(messages[0].message==WM_MOUSEMOVE&&messages[1].message==WM_LBUTTONDOWN&&messages[2].message==WM_LBUTTONUP);
    check(messages[1].buttons==MK_LBUTTON&&messages[2].buttons==0&&LOWORD(messages[1].point)==59&&HIWORD(messages[1].point)==27);
    capture=nullptr;messages.clear();clip_rect={0,0,1,1};input.click(candidate,.5f,.5f);check(messages.size()==3&&calls.empty());
    messages.clear();clip_rect={-128,0,128,128};fullscreen=true;input.click(candidate,.5f,.5f);check(messages.size()==3&&calls.empty());
    fullscreen=false;capture=foreground;messages.clear();action=ImeAction::Invoked;input.click(candidate,.5f,.5f);check(messages.empty()&&calls.empty());
    action=ImeAction::Failed;input.click(candidate,.5f,.5f);check(messages.empty()&&calls.empty()); // no duplicate after ambiguous UIA failure
    action=ImeAction::Unavailable;fail_down=true;input.click(candidate,.5f,.5f);
    check(messages.size()==3&&messages.back().message==WM_LBUTTONUP&&calls.empty());fail_down=false;
    messages.clear();covered=true;const auto attempts=actions;input.click(candidate,.5f,.5f);
    check(actions==attempts&&messages.empty()&&calls.empty());covered=false;capture=nullptr;
    // Actual discovery accepts standard IME events without a class/process name.
    ImeEventState state;ImeEventWindow item{candidate_window,foreground,focus,candidate_pid,candidate_thread};RECT region{};
    state.observe(EVENT_OBJECT_SHOW,item);check(state.entries.empty()); // ordinary popup is not an IME
    state.observe(EVENT_OBJECT_IME_SHOW,item);check(state.entries.size()==1&&ime_event_region(state.entries[0],foreground,region));
    candidate_pid++;check(!ime_event_region(item,foreground,region));candidate_pid--;
    candidate_thread++;check(!ime_event_region(item,foreground,region));candidate_thread--;
    const auto saved_focus=focus;focus=nullptr;check(!ime_event_region(item,foreground,region));focus=saved_focus;
    covered=true;check(!ime_event_region(item,foreground,region));covered=false;
    candidate_style=WS_POPUP|WS_CAPTION;check(!ime_event_region(item,foreground,region));candidate_style=WS_POPUP;
    const auto saved_rect=candidate_rect;candidate_rect={200,10,260,60};
    check(!ime_event_region(item,foreground,region));owner=foreground;
    check(ime_event_region(item,foreground,region)); // owned popup outside the app is valid
    candidate_rect={0,0,4096,2160};check(!ime_event_region(item,foreground,region));
    candidate_rect={0,0,64,900};check(ime_event_region(item,foreground,region)); // vertical candidate list
    candidate_rect=saved_rect;owner=nullptr;
    state.observe(EVENT_OBJECT_IME_CHANGE,item);check(state.entries.size()==1);
    state.observe(EVENT_OBJECT_IME_HIDE,item);check(state.entries.empty());
    state.observe(EVENT_OBJECT_IME_SHOW,item);state.observe(EVENT_OBJECT_HIDE,item);check(state.entries.empty());
    state.observe(EVENT_OBJECT_IME_SHOW,item);state.observe(EVENT_OBJECT_DESTROY,item);check(state.entries.empty());
    for(unsigned i=0;i<40;++i){auto next=item;next.window=reinterpret_cast<HWND>(uintptr_t(100+i));state.observe(EVENT_OBJECT_IME_SHOW,next);}
    check(state.entries.size()==16);state.observe(EVENT_SYSTEM_FOREGROUND,item);check(state.entries.empty());
    // Denied motion must preserve the session and admit fresh motion later.
    results={0};input.apply({EventKind::Move,0,.2f,.3f,0,0});
    input.apply({EventKind::Move,0,.6f,.7f,0,0});check(calls.size()==2);
    // Partial batches retain only release edges, never replay the click.
    calls.clear();results={1,0};input.apply({EventKind::Button,0,.5f,.5f,0,1});
    check(calls.size()==2&&calls[0].size()==2&&calls[1][0].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    results={0};check(!input.tick());check(calls.back()[0].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    check(input.tick());check(calls.back()[0].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    // Failed key-down and key-up both release conservatively.
    calls.clear();results={0,0};input.apply({EventKind::Key,0,0,0,0x1e,1});
    check(calls.size()==2&&(calls[1][0].ki.dwFlags&KEYEVENTF_KEYUP));
    check(input.tick());check(calls.back()[0].ki.dwFlags&KEYEVENTF_KEYUP);
    input.apply({EventKind::Key,0,0,0,0x1e,1});results={0,0};
    input.apply({EventKind::Key,0,0,0,0x1e,0});check(input.tick());
    check(calls.back()[0].ki.dwFlags&KEYEVENTF_KEYUP);
    calls.clear();results={0,0};input.apply({EventKind::Unicode,0,0,0,0xd83d,1});
    check(calls[1][0].ki.dwFlags==(KEYEVENTF_UNICODE|KEYEVENTF_KEYUP));
    check(input.tick());check(calls.back()[0].ki.dwFlags==(KEYEVENTF_UNICODE|KEYEVENTF_KEYUP));
    // Switching to an unavailable secure desktop must discard new presses.
    calls.clear();input.apply({EventKind::Key,0,0,0,0x1e,1});active=2;denied=true;
    input.apply({EventKind::Key,0,0,0,0x30,1});
    check(calls.size()==2&&(calls.back()[0].ki.dwFlags&KEYEVENTF_KEYUP));
    denied=false;check(input.tick());check(attached==2&&(attach_access&GENERIC_WRITE));
    input.apply({EventKind::Key,0,0,0,0x30,1});check(calls.back()[0].ki.wScan==0x30);
    input.release();check(calls.back()[0].ki.dwFlags&KEYEVENTF_KEYUP);
    // Absolute events on secondary screens use their own physical rectangle.
    calls.clear();active=1;check(input.tick());
    input.displays({{-128,0,128,128,128,128},{0,0,128,128,128,128}});
    input.apply({EventKind::Move,1,0,0,0,0});
    check(calls.back()[0].mi.dx==32896&&calls.back()[0].mi.dy==256);
    input.apply({EventKind::Button,1,1,1,0,1});
    check(calls.back()[0].mi.dx==65408&&calls.back()[0].mi.dy==65280);
    input.displays({{-128,0,128,128,128,128}});
    check(calls.back()[0].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    const auto before=calls.size();input.apply({EventKind::Button,1,1,1,0,1});check(calls.size()==before);
    input.apply({EventKind::Move,0,.2f,.2f});
    auto unchanged=calls.size();input.apply({EventKind::Move,0,.2f,.2f});check(calls.size()==unchanged);
    MSLLHOOKSTRUCT mouse{};mouse.pt={100,100};mouse.dwExtraInfo=MouseActivity::injection_tag;
    check(mouse_hook!=nullptr);simulate_mouse(mouse);
    input.prefer_pc(true);input.apply({EventKind::Button,0,.3f,.3f,0,1});check(calls.size()==unchanged+1);
    mouse.dwExtraInfo=0;mouse.pt.x+=16;
    simulate_mouse(mouse);
    unchanged=calls.size();input.apply({EventKind::Move,0,.5f,.5f});check(calls.size()==unchanged);
    input.apply({EventKind::Wheel,0,0,0,0,120});check(calls.size()==unchanged);
    input.apply({EventKind::Button,0,.5f,.5f,0,0});
    check(calls.size()==unchanged+1&&calls.back().size()==1&&calls.back()[0].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    unchanged=calls.size();input.apply({EventKind::Button,0,.6f,.6f,0,1});check(calls.size()==unchanged);
    input.prefer_pc(false);input.apply({EventKind::Move,0,.8f,.8f});check(calls.size()==unchanged+1);
    input.apply({EventKind::Wheel,0,0,0,120,-120});
    check(calls.back().size()==3&&(calls.back()[0].mi.dwFlags&MOUSEEVENTF_ABSOLUTE)&&calls.back()[1].mi.dwFlags==MOUSEEVENTF_HWHEEL&&calls.back()[2].mi.dwFlags==MOUSEEVENTF_WHEEL);
    check(simulate_mouse(mouse)==1);
    mouse.dwExtraInfo=MouseActivity::injection_tag;check(simulate_mouse(mouse)==0);
    // VR warps reset the native baseline without contributing PC displacement.
    input.prefer_pc(true);mouse.pt={1000,1000};check(simulate_mouse(mouse)==0);
    mouse.dwExtraInfo=0;mouse.pt.x+=3;
    check(simulate_mouse(mouse)==1); // small native motion cannot move a VR-owned cursor
    mouse.pt.x=998;check(simulate_mouse(mouse)==1); // measure from the admitted 1000, not rejected 1003
    mouse.pt.x=1004;check(simulate_mouse(mouse)==1);
    mouse.pt.x=1007;check(simulate_mouse(mouse)==0); // suppressed relative deltas: 3 - 2 + 4 + 7
    unchanged=calls.size();input.apply({EventKind::Move,0,.81f,.8f});
    check(calls.size()==unchanged);
    input.apply({EventKind::Release});
    unchanged=calls.size();input.apply({EventKind::Move,0,.2f,.2f}); // first ray after release only primes
    input.apply({EventKind::Button,0,.2f,.2f,0,1});
    input.apply({EventKind::Wheel,0,0,0,0,120});
    check(calls.size()==unchanged); // stale click/scroll must not snap back either
    std::cout<<"{\"status\":\"passed\",\"production_methods\":true,\"input_injected\":false,\"desktop_switched\":false,\"event_log_written\":false}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Windows SDK is required')
    owner = PROJECT / 'pc/desktop'
    header = production_source(owner / 'input.hpp')
    header = '\n'.join(line for line in header.splitlines() if not line.startswith('#'))
    source = production_source(owner / 'input.cpp')
    source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    desktop = production_source(owner / 'capture.cpp')
    cpp = CPP.replace('DESKTOP_CLASS', method(production_source(owner / 'windows.hpp'), 'class InputDesktop {'))
    cpp = cpp.replace('ATTACH_METHOD', method(desktop, 'bool InputDesktop::attach()'))
    cpp = cpp.replace('CHANGED_METHOD', method(desktop, 'bool InputDesktop::changed()'))
    for token, filename in [('ARBITER_HEADER', 'pointer_arbiter.hpp'), ('ACTIVITY_HEADER', 'mouse_activity.hpp'), ('ACTIVITY_SOURCE', 'mouse_activity.cpp')]:
        value = production_source(owner / filename)
        cpp = cpp.replace(token, '\n'.join(line for line in value.splitlines() if not line.startswith('#')))
    cpp = cpp.replace('INPUT_HEADER', header).replace('INPUT_SOURCE', source)
    ime = production_source(owner / 'ime_capture.hpp')
    cpp = cpp.replace('IME_HEADER', '\n'.join(line for line in ime.splitlines() if not line.startswith('#')))
    cpp = cpp.replace('IME_CONTAINS', method(production_source(owner / 'ime_capture.cpp'), 'bool ImeCandidate::contains('))
    uia = production_source(owner / 'ime_uia.hpp')
    cpp = cpp.replace('IME_UIA_HEADER', '\n'.join(line for line in uia.splitlines() if not line.startswith('#')))
    clicks = production_source(owner / 'ime_click.cpp')
    cpp = cpp.replace('IME_CLICK_SOURCE', '\n'.join(line for line in clicks.splitlines() if not line.startswith('#include')))
    events = production_source(owner / 'ime_events.hpp')
    cpp = cpp.replace('IME_EVENTS_HEADER', '\n'.join(line for line in events.splitlines() if not line.startswith('#')))
    events_source = production_source(owner / 'ime_events.cpp')
    cpp = cpp.replace('IME_EVENT_METHODS', method(events_source, 'HWND ime_focus(') + '\n' + method(events_source, 'bool ime_event_current('))
    region = production_source(owner / 'ime_region.cpp')
    region = region.split('namespace surfxr::transport::desktop {', 1)[1].rsplit('}', 1)[0]
    cpp = cpp.replace('IME_REGION_SOURCE', region)
    staging = PROJECT.parent / 'build/pc/input-tests'
    staging.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fixture-', dir=staging) as directory:
        root = Path(directory)
        (root / 'check.cpp').write_text(cpp, encoding='utf-8')
        (root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(desktop_input_check LANGUAGES CXX)\n'
            'add_executable(desktop-input-check check.cpp)\n'
            'target_compile_features(desktop-input-check PRIVATE cxx_std_17)\n'
            'target_link_libraries(desktop-input-check PRIVATE user32)\n'
            f'target_include_directories(desktop-input-check PRIVATE {include_directories(PROJECT)})\n')
        for args in (['cmake', '-S', str(root), '-B', str(root / 'build'), '-A', 'x64'],
                     ['cmake', '--build', str(root / 'build'), '--config', 'Release']):
            p = subprocess.run(args, capture_output=True, text=True, errors='replace')
            if p.returncode:
                raise RuntimeError(p.stdout + p.stderr)
        p = subprocess.run([str(root / 'build/Release/desktop-input-check.exe')],
                           capture_output=True, text=True, timeout=10, check=True)
        print(json.dumps(json.loads(p.stdout)))


if __name__ == '__main__':
    main()
