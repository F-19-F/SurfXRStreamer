#pragma once
#include "surfxr/forwarding.hpp"
#include "openvr_driver_wrap.h"
#include <cstring>
namespace surfxr {
struct BooleanInput {
    using Update = vr::EVRInputError (*)(void*, vr::VRInputComponentHandle_t, bool, double);
    void* instance = nullptr;
    Update function = nullptr;
    const char* adaptation = nullptr;
    vr::EVRInputError update(vr::VRInputComponentHandle_t handle, bool value, double offset) const {
        return function ? function(instance, handle, value, offset)
                        : vr::VRInputError_InvalidHandle;
    }
};
namespace boolean_input {
    inline BooleanInput entry(void* instance) {
        if (!instance)
            return {};
        void** table = nullptr;
        std::memcpy(&table, instance, sizeof(table));
        return { instance, reinterpret_cast<BooleanInput::Update>(table[1]) };
    }
    inline BooleanInput select(
        void* adapter,
        std::uintptr_t create,
        std::uintptr_t update,
        const forwarding::Reader& original,
        const forwarding::Reader& live
    ) {
        auto fallback = entry(adapter);
        const auto a = forwarding::x64(create, original), b = forwarding::x64(update, original),
                   current = forwarding::x64(create, live);
        fallback.adaptation = "boolean forwarding rejected: runtime semantics could not be proven; "
                              "public API retained";
        if (!a || !b || !current || !(a->object == b->object) || !(a->object == current->object)
            || a->slot != current->slot)
            return fallback;
        const auto target
            = forwarding::resolve(*b, reinterpret_cast<std::uintptr_t>(adapter), live);
        const auto creator
            = forwarding::resolve(*a, reinterpret_cast<std::uintptr_t>(adapter), live);
        unsigned char byte = 0;
        if (!target || !creator || target->instance != creator->instance
            || target->instance == reinterpret_cast<std::uintptr_t>(adapter)
            || target->slot == creator->slot || !live(target->function, &byte, 1, true)
            || !live(creator->function, &byte, 1, true))
            return fallback;
        return { reinterpret_cast<void*>(target->instance),
                 reinterpret_cast<BooleanInput::Update>(target->function),
                 "boolean forwarding dynamically resolved from matching runtime image; unsupported "
                 "code falls back to public API" };
    }
}
BooleanInput ResolveBooleanInput(vr::IVRDriverInput* input);
}
