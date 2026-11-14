#pragma once
#include "controller_status.hpp"

namespace surfxr::xr_detail {
class SteamVrControllers {
public:
    bool poll(ControllerStatus& result,std::uint64_t now,bool changed);
private:
    std::uint64_t next_sample_=0;
};
}
