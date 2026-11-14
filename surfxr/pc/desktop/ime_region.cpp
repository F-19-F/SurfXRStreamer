#include "ime_region.hpp"
#include "ime_events.hpp"
namespace surfxr::transport::desktop {
bool ime_region_size(const RECT& rect){
    const auto width=static_cast<long long>(rect.right)-rect.left,height=static_cast<long long>(rect.bottom)-rect.top;
    return width>=8&&height>=8&&width<=2048&&height<=1024;
}
bool ime_owned_by(HWND window,HWND foreground){
    // Candidate popups may sit outside the application's client rectangle.
    // Walk a bounded owner chain; process equality alone is insufficient.
    HWND owner=window;
    for(unsigned i=0;i<16&&owner;++i){
        owner=GetWindow(owner,GW_OWNER);
        if(owner&&(owner==foreground||IsChild(foreground,owner)))return true;
    }
    return false;
}
bool ime_region_related(HWND window,HWND foreground,const RECT& rect){
    if(!window||!foreground||window==foreground||!IsWindowVisible(window)||IsIconic(window))return false;
    if(ime_owned_by(window,foreground))return true;
    RECT app{},overlap{};
    return GetWindowRect(foreground,&app)&&IntersectRect(&overlap,&app,&rect);
}
bool ime_region_visible(HWND window,const RECT& rect){
    if(!ime_region_size(rect))return false;
    for(int y=1;y<=3;++y)for(int x=1;x<=3;++x){
        const auto hit=WindowFromPoint({rect.left+(rect.right-rect.left)*x/4,rect.top+(rect.bottom-rect.top)*y/4});
        if(hit!=window&&!IsChild(window,hit))return false;
    }
    return true;
}
bool ime_event_region(const ImeEventWindow& item,HWND foreground,RECT& rect){
    if(!ime_event_current(item,foreground)||!GetWindowRect(item.window,&rect)||!ime_region_related(item.window,foreground,rect))return false;
    const auto style=GetWindowLongPtrW(item.window,GWL_STYLE);
    return (style&WS_POPUP)&&!(style&WS_CAPTION)&&ime_region_visible(item.window,rect);
}
}
