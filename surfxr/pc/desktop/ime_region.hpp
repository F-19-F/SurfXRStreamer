#pragma once
#include "windows.hpp"
namespace surfxr::transport::desktop {
struct ImeEventWindow;
// Shared native/UIA selection rules. No localized names or executable names.
bool ime_region_size(const RECT& rect);
bool ime_region_visible(HWND window,const RECT& rect);
bool ime_region_related(HWND window,HWND foreground,const RECT& rect);
bool ime_owned_by(HWND window,HWND foreground);
bool ime_event_region(const ImeEventWindow&,HWND foreground,RECT& rect);
}
