#pragma once
#include "connection.hpp"

namespace surfxr::launcher {
// Public diagnostics come from local fixed reasons, never arbitrary child text.
Json session_failure(const Json& report, int exit_code);
// Observing readiness has no process ownership and must never stop a viewer.
Json observe_start(const std::function<Json()>& read_status, const std::string& mode,
                   std::chrono::milliseconds timeout = std::chrono::seconds(90));
}
