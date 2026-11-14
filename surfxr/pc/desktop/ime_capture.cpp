#include "ime_capture.hpp"
#include "ime_uia.hpp"
#include "ime_events.hpp"
#include "ime_region.hpp"
#include "surfxr/keyboard_image.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <iostream>

namespace surfxr::transport::desktop {
namespace {
struct Search {HWND foreground=nullptr,candidate=nullptr;unsigned status=1;};
BOOL CALLBACK find(HWND window,LPARAM param){
    auto& s=*reinterpret_cast<Search*>(param);
    if(!IsWindowVisible(window)||IsIconic(window))return TRUE;
    wchar_t name[160]{};GetClassNameW(window,name,160);
    const std::wstring cls=name;
    if(cls!=L"Microsoft.IME.UIManager.CandidateWindow.Host"&&
       cls!=L"MSCandUIWindow_Candidate"&&cls!=L"IME_Candidate"&&
       cls!=L"SogouPY_CandWnd"&&cls!=L"SoPY_Comp")return TRUE;
    s.status=2;
    RECT rect{};
    if(!GetWindowRect(window,&rect)||!ime_region_related(window,s.foreground,rect)||!ime_region_size(rect))return TRUE;
    if(!ime_region_visible(window,rect)){s.status=3;return TRUE;}
    s.candidate=window;return FALSE;
}
struct Dc {HDC value=nullptr;HWND window=nullptr;bool memory=false;
    ~Dc(){if(value){if(memory)DeleteDC(value);else ReleaseDC(window,value);}}};
struct Bitmap {HBITMAP value=nullptr;~Bitmap(){if(value)DeleteObject(value);}};
struct Selection {HDC dc;HGDIOBJ old;~Selection(){if(old)SelectObject(dc,old);}};
}
bool ImeCandidate::contains(float u,float v)const{
    if(!(u>=0&&u<=1&&v>=0&&v<=1)||GetForegroundWindow()!=foreground)return false;
    DWORD current_process=0;const auto current_thread=GetWindowThreadProcessId(window,&current_process);
    if((process&&process!=current_process)||(thread&&thread!=current_thread))return false;
    RECT current{};const auto& host=IsRectEmpty(&host_rect)?rect:host_rect;
    if(!IsWindowVisible(window)||!GetWindowRect(window,&current)||!EqualRect(&host,&current))return false;
    POINT point{rect.left+static_cast<LONG>(std::lround(u*(rect.right-rect.left-1))),rect.top+static_cast<LONG>(std::lround(v*(rect.bottom-rect.top-1)))};
    const auto hit=WindowFromPoint(point);return hit==window||IsChild(window,hit);
}
ImeCandidate capture_ime_candidate(unsigned* status,ImeObserver* observer){
    unsigned code=0;struct Report {unsigned* output;unsigned& code;~Report(){if(output)*output=code;}}report{status,code};
    const auto prior=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    struct Restore {DPI_AWARENESS_CONTEXT prior;~Restore(){if(prior)SetThreadDpiAwarenessContext(prior);}}restore{prior};
    Search search;search.foreground=GetForegroundWindow();
    if(!search.foreground)return {};
    RECT rect{},host_rect{};unsigned discovery=0;
    std::vector<HWND> hosts;
    if(observer){
        const auto entries=observer->windows();
        for(auto it=entries.rbegin();it!=entries.rend();++it){
            if(!ime_event_current(*it,search.foreground))continue;
            if(!GetWindowRect(it->window,&rect)||!ime_region_related(it->window,search.foreground,rect))continue;
            // A standard IME event is independent of class, process or language.
            // Only small popup windows can use their complete native bounds.
            if(ime_event_region(*it,search.foreground,rect)){search.candidate=it->window;discovery=1;break;}
            hosts.push_back(it->window);
        }
    }
    if(!search.candidate){
        EnumDesktopWindows(nullptr,find,reinterpret_cast<LPARAM>(&search));
        if(search.candidate){discovery=2;if(!GetWindowRect(search.candidate,&rect))return {};}
        else {if(!ime_uia_region(search.foreground,search.candidate,rect,hosts)){code=search.status;return {};}discovery=3;}
    }
    code=3;
    if(!GetWindowRect(search.candidate,&host_rect))return {};
    if(!ime_region_visible(search.candidate,rect))return {};
    code=4;
    const int source_width=rect.right-rect.left,source_height=rect.bottom-rect.top;
    if(source_width<=0||source_height<=0)return {};
    const float scale=std::min({1.f,float(keyboard_image::max_width)/source_width,float(keyboard_image::max_height)/source_height});
    const auto width=std::max(1,static_cast<int>(source_width*scale)),height=std::max(1,static_cast<int>(source_height*scale));
    Dc screen{GetDC(nullptr),nullptr,false};if(!screen.value)return {};
    Dc memory{CreateCompatibleDC(screen.value),nullptr,true};if(!memory.value)return {};
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;Bitmap bitmap{CreateDIBSection(screen.value,&info,DIB_RGB_COLORS,&pixels,nullptr,0)};
    if(!bitmap.value||!pixels)return {};
    Selection select{memory.value,SelectObject(memory.value,bitmap.value)};
    SetStretchBltMode(memory.value,COLORONCOLOR);
    if(!StretchBlt(memory.value,0,0,width,height,screen.value,rect.left,rect.top,source_width,source_height,SRCCOPY|CAPTUREBLT))return {};
    GdiFlush();
    // Focus/window changes invalidate the capture; don't carry the old app's list.
    RECT current{};
    code=5;
    if(GetForegroundWindow()!=search.foreground||!IsWindowVisible(search.candidate)||
       !GetWindowRect(search.candidate,&current)||!EqualRect(&host_rect,&current))return {};
    auto image=keyboard_image::encode(static_cast<unsigned>(width),static_cast<unsigned>(height),static_cast<const std::uint8_t*>(pixels));
    code=image.empty()||image.size()+8>keyboard_image::max_bytes?6:7;
    if(code==6)return {};
    DWORD process=0;const auto thread=GetWindowThreadProcessId(search.candidate,&process);
    if(!process||!thread)return {};
    return {std::move(image),search.candidate,search.foreground,rect,host_rect,discovery,process,thread};
}
int ime_probe(unsigned seconds){
    InputDesktop desktop;desktop.attach();ImeObserver observer;
    ImeCandidate previous;
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    do{
        if(desktop.changed())desktop.attach();
        unsigned status=0;const auto candidate=capture_ime_candidate(&status,&observer);
        std::cout<<"{\"status\":"<<status<<",\"discovery\":"<<candidate.discovery<<",\"same_geometry\":"<<candidate.same_geometry(previous)<<",\"same_pixels\":"<<(candidate.image==previous.image)<<",\"bytes\":"<<candidate.image.size()<<",\"width\":"
            <<candidate.rect.right-candidate.rect.left<<",\"height\":"<<candidate.rect.bottom-candidate.rect.top<<"}\n"<<std::flush;
        previous=candidate;
        Sleep(250);
    }while(std::chrono::steady_clock::now()<until);
    return 0;
}
}
