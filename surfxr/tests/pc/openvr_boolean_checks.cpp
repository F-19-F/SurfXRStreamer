#include "surfxr_server/BooleanInput.h"
#include "surfxr_server/FrameInput.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include "surfxr/runtime_memory.hpp"

extern "C" unsigned long long PathStringToHash(const char* text) {
    unsigned long long result = 14695981039346656037ull;
    while (*text) result = (result ^ static_cast<unsigned char>(*text++)) * 1099511628211ull;
    return result;
}
namespace {
void require(bool value) { if (!value) throw std::runtime_error("boolean forwarding regression"); }
using Handle = vr::VRInputComponentHandle_t;
struct Runtime {
    void** table;
    std::map<Handle, bool> buttons;
    Handle latest = 0;
    double offset = 0;
};
struct Adapter { void** table; Runtime* target; };
vr::EVRInputError update(void* instance, Handle handle, bool value, double offset) {
    auto& runtime = *static_cast<Runtime*>(instance);
    const auto found = runtime.buttons.find(handle);
    if (found == runtime.buttons.end()) return vr::VRInputError_InvalidHandle;
    found->second = value; runtime.latest = handle; runtime.offset = offset;
    return vr::VRInputError_None;
}
vr::EVRInputError pose_detour(void* instance, Handle handle, bool value, double offset) {
    // The folded IVRServerDriverHost pose adapter accepts a uint32 device ID.
    // A detour with that signature destroys the upper half of an input handle.
    return update(static_cast<Adapter*>(instance)->target, static_cast<std::uint32_t>(handle), value, offset);
}
}
int main() try {
    using namespace surfxr::boolean_input;
    void* runtime_table[] = {reinterpret_cast<void*>(update), reinterpret_cast<void*>(update)};
    void* adapter_table[] = {nullptr, reinterpret_cast<void*>(pose_detour)};
    Runtime runtime{runtime_table, {}};
    Adapter adapter{adapter_table, &runtime};
    // Instruction fixtures are test inputs, not production signatures.
    const std::array<unsigned char,10> create_code{{0x48,0x8b,0x49,0x08,0x48,0x8b,0x01,0x48,0xff,0x20}};
    const std::array<unsigned char,11> update_code{{0x48,0x8b,0x49,0x08,0x48,0x8b,0x01,0x48,0xff,0x60,0x08}};
    surfxr::RuntimeMemory memory;
    const surfxr::forwarding::Reader read=[&](std::uintptr_t address,void* out,std::size_t count,bool code){
        if(address>=0x1000&&address-0x1000<create_code.size()){
            if(count>create_code.size()-(address-0x1000))return false;
            std::memcpy(out,create_code.data()+address-0x1000,count);return true;
        }
        if(address>=0x2000&&address-0x2000<update_code.size()){
            if(count>update_code.size()-(address-0x2000))return false;
            std::memcpy(out,update_code.data()+address-0x2000,count);return true;
        }
        return memory(address,out,count,code);
    };
    const auto broken = entry(&adapter);
    const auto repaired = select(&adapter,0x1000,0x2000,read,read);
    require(repaired.instance == &runtime);
    for (unsigned hand = 0; hand < 2; ++hand) {
        unsigned component = 0;
        const std::string prefix = hand ? "/user/hand/right/input/" : "/user/hand/left/input/";
        const auto check = [&](const char* name, const char* suffix) {
            const auto path = prefix + name + "/" + suffix;
            const auto mapping = FrameInput(PathStringToHash(path.c_str()), hand == 0);
            require(mapping.type == ButtonType::Binary && mapping.steamvr_paths.size() == 1);
            const Handle handle = (Handle(hand + 1) << 32) | ++component;
            runtime.buttons[handle] = false;
            require(broken.update(handle, true, .125) == vr::VRInputError_InvalidHandle);
            for (bool down : {true, false}) {
                require(repaired.update(handle, down, .125) == vr::VRInputError_None);
                require(runtime.latest == handle && runtime.buttons.at(handle) == down && runtime.offset == .125);
            }
        };
        for (const char* name : {"bumper", "trigger", "squeeze", "thumbstick", "system"})
            for (const char* suffix : {"click", "touch"}) check(name, suffix);
        const std::vector<const char*> names = hand ? std::vector<const char*>{"a", "b", "x", "y", "menu"}
            : std::vector<const char*>{"dpad_up", "dpad_down", "dpad_left", "dpad_right", "view"};
        for (const auto* name : names) for (const auto* suffix : {"click", "touch"}) check(name, suffix);
        // The offline proximity guard uses the same boolean path, outside input mappings.
        const Handle guard = (Handle(hand + 1) << 32) | 31;
        runtime.buttons[guard] = false;
        require(broken.update(guard, true, 0) == vr::VRInputError_InvalidHandle);
        require(repaired.update(guard, true, 0) == vr::VRInputError_None);
    }
    // Unreadable/unproven code and missing targets must preserve the public API.
    const surfxr::forwarding::Reader unavailable=[](std::uintptr_t,void*,std::size_t,bool){return false;};
    require(select(&adapter,0x1000,0x2000,unavailable,read).instance==&adapter);
    require(select(&adapter,0x1000,0x2000,read,unavailable).instance==&adapter);
    adapter.target=nullptr;
    require(select(&adapter,0x1000,0x2000,read,read).instance==&adapter);
    require(surfxr::ResolveBooleanInput(nullptr).instance == nullptr);
    std::cout << "Shared pose-hook corruption reproduced; both hands' 40 boolean paths, system/dpad edges, "
                 "proximity, 64-bit handles, time offsets and unknown-ABI fallback passed. No SteamVR connection.\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
