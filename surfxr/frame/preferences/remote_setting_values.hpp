#pragma once
#include <algorithm>
#include <array>
namespace surfxr::setting_values {
inline constexpr std::array<int,9> framerates{24,30,60,72,80,90,96,100,120};
inline bool known_rate(double value) { return std::find(framerates.begin(),framerates.end(),value) != framerates.end(); }
}
