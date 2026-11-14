#pragma once
#include "Paths.h"
#include "bindings.h"
#include <string>
#include <vector>

// The built-in Valve profile uses grip; SURFXR's wire keeps squeeze.
// Native mode creates exactly one component per wire input, including menu.
inline ButtonInfo FrameInput(uint64_t id, bool left) {
    const std::string hand = left ? "/user/hand/left" : "/user/hand/right";
    static const char* leftComponents[] = {
        "/input/dpad_up/click", "/input/dpad_up/touch", "/input/dpad_down/click", "/input/dpad_down/touch",
        "/input/dpad_left/click", "/input/dpad_left/touch", "/input/dpad_right/click", "/input/dpad_right/touch",
        "/input/view/click", "/input/view/touch"
    };
    static const char* rightComponents[] = {
        "/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch",
        "/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch", "/input/menu/click", "/input/menu/touch"
    };
    static const char* commonComponents[] = {
        "/input/bumper/click", "/input/bumper/touch",
        "/input/trigger/click", "/input/trigger/touch", "/input/grip/click", "/input/grip/touch",
        "/input/thumbstick/click", "/input/thumbstick/touch", "/input/system/click", "/input/system/touch",
        "/input/trigger/value", "/input/grip/value", "/input/thumbstick/x", "/input/thumbstick/y"
    };
    for (unsigned i=0;i<24;++i) {
        const auto* component=i<10?(left?leftComponents[i]:rightComponents[i]):commonComponents[i-10];
        std::string wire(component);
        if (wire.find("/grip/") != std::string::npos) wire.replace(7, 4, "squeeze");
        if (PathStringToHash((hand + wire).c_str()) == id) {
            const auto scalar = wire.find("/value") != std::string::npos || wire.compare(wire.size()-2, 2, "/x") == 0 || wire.compare(wire.size()-2, 2, "/y") == 0;
            return { { component }, scalar ? (wire.find("/value") != std::string::npos ? ButtonType::ScalarOneSided : ButtonType::ScalarTwoSided) : ButtonType::Binary };
        }
    }
    return {};
}
