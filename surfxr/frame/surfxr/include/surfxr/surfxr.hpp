#pragma once
#include "surfxr/session.hpp"

namespace surfxr::transport {
SessionResult run(const SessionOptions&,const SessionCallbacks&,const std::atomic_bool*);
SessionResult run_desktop(const SessionOptions&,const SessionCallbacks&,const std::atomic_bool*);
SessionResult run_modes(const SessionOptions&,const SessionCallbacks&,const std::atomic_bool*);
}
