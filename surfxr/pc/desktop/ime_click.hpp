#pragma once
#include "ime_capture.hpp"
namespace surfxr::transport::desktop {
// Private routing for a validated candidate; never releases another app's capture.
bool ime_pointer_constrained(HWND foreground,HWND candidate);
bool ime_window_click(const ImeCandidate&,float u,float v);
}
