#pragma once
#include <string>
#include <string_view>

namespace surfxr {
// Writers use the current product tag. Read the historical two-byte tag only
// for local preference migration; the next save emits the current spelling.
inline bool preference_format_matches(std::string_view token, std::string_view kind) {
    const auto suffix = std::string("_FRAME_") + std::string(kind);
    // Legacy serialized headers remain accepted so upgrades preserve saved preferences.
    if (token == "SURFXR" + suffix || token == "ALVREVO" + suffix) return true;
    return token.size() == suffix.size() + 2 &&
        static_cast<unsigned char>(token[0]) == 0x56 &&
        static_cast<unsigned char>(token[1]) == 0x44 && token.substr(2) == suffix;
}
}
