#pragma once
#include "surfxr/vr_ui.hpp"
#include <array>
#include <cstdint>
#include <string>
namespace surfxr::ui_detail {
bool localized_size(const std::string& text,unsigned scale,UiTextSize& size);
bool localized_text(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,
    unsigned x,unsigned y,const std::string& text,unsigned scale,std::array<std::uint8_t,4> color);
}
