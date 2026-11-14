#pragma once
#include "viewer_report.hpp"

namespace surfxr::viewer {
// References are owned by main and remain alive through StopAndJoin.
struct ReceiverWorker {
    ViewerReport& result;
    ViewerOptions& opts;
    std::array<surfxr::LatestFrameSlot,4>& slots;
    std::array<std::atomic_bool,4>& available;
    surfxr::XrDisplay& display;
    std::shared_ptr<surfxr::SessionInputQueue>& input;
    std::shared_ptr<surfxr::SessionControlQueue>& control;
    surfxr::VrClientState& vr;
    surfxr::RemoteSettings& settings;
    std::atomic_bool& controllers_enabled;
    std::atomic_bool& microphone_enabled;
    surfxr::BatteryMonitor& battery_monitor;
    surfxr::TestHeadMotion& test_head_motion;
    surfxr::ViewerDecodeTelemetrySlot& decode_telemetry;
    DesktopFrameDemand& desktop_demand;
    void run();
};
}
