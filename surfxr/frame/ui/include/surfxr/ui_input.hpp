#pragma once
#include "surfxr/input_wire.hpp"
#include "surfxr/vr_ui.hpp"
namespace surfxr {
ScanCode ui_scan_code(UiKey key);
void queue_ui_input(const UiEvent& event,SessionInputQueue& queue);
void ui_input_self_test();
}
