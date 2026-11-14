#pragma once
#include <array>
#include <cstdint>

namespace surfxr::tests::color_bars {
constexpr unsigned width=1024,height=256;
constexpr std::array<std::array<std::uint8_t,3>,16> colors{{
    {0,0,0},{16,16,16},{32,32,32},{64,64,64},
    {128,128,128},{192,192,192},{235,235,235},{255,255,255},
    {255,0,0},{0,255,0},{0,0,255},{0,255,255},
    {255,0,255},{255,255,0},{64,128,192},{192,128,64}}};
constexpr unsigned bar_width=width/static_cast<unsigned>(colors.size());
}
