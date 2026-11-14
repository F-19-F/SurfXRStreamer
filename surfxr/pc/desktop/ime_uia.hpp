#pragma once
#include "windows.hpp"
#include <vector>
namespace surfxr::transport::desktop {
struct ImeCandidate;
enum class ImeAction {Unavailable,Invoked,Failed};
// Failed includes ambiguous provider errors: never follow with another click.
ImeAction ime_uia_invoke(const ImeCandidate&,float u,float v);
// Nonlocalized candidate ROI from eligible hosts, independent of their process.
bool ime_uia_region(HWND foreground,HWND& window,RECT& rect,const std::vector<HWND>& preferred={});
}
