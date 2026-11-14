#pragma once
#include "windows.hpp"
namespace surfxr::transport::desktop {
class ImeObserver;
// Standard IME events, explicit UIA candidate ROI, or legacy class fallback.
struct ImeCandidate {
    Bytes image;HWND window=nullptr,foreground=nullptr;RECT rect{},host_rect{};
    unsigned discovery=0; // 1: standard IME event, 2: legacy class, 3: UIA ROI
    DWORD process=0,thread=0;
    bool same_geometry(const ImeCandidate& other)const {
        return window==other.window&&foreground==other.foreground&&process==other.process&&thread==other.thread&&
            EqualRect(&rect,&other.rect)&&EqualRect(&host_rect,&other.host_rect);
    }
    bool same_target(const ImeCandidate& other)const {
        // Pixels can change with animated backgrounds, hover and caret blink.
        // A ray selects the current item at the displayed window coordinates.
        return !image.empty()&&!other.image.empty()&&same_geometry(other);
    }
    bool contains(float u,float v)const;
};
ImeCandidate capture_ime_candidate(unsigned* status=nullptr,ImeObserver* observer=nullptr);
int ime_probe(unsigned seconds);
}
