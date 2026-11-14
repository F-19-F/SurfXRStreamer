#pragma once
#include "desktop_demand.hpp"
#include "surfxr/session.hpp"
#include "surfxr/video_decoder.hpp"
#include "surfxr/async_video_decoder.hpp"
#include "surfxr/xr_display.hpp"
#include "surfxr/input_wire.hpp"
#include "surfxr/ui_input.hpp"
#include "surfxr/vr_client_state.hpp"
#ifdef SURFXR_HAVE_MICROPHONE
#include "surfxr/microphone_capture.hpp"
#endif
#include "surfxr/layout_store.hpp"
#include "surfxr/settings_store.hpp"
#include "surfxr/viewer_status.hpp"
#include "surfxr/test_head_motion.hpp"
#include "surfxr/cpu_affinity.hpp"
#include "surfxr/network_binding.hpp"
#include "surfxr/media_cadence.hpp"
#include "surfxr/input_preferences.hpp"
#include "surfxr/battery.hpp"
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
#include "surfxr/audio_output.hpp"
#endif
#include <arpa/inet.h>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>


namespace surfxr::viewer {
struct ViewerOptions {
    surfxr::SessionOptions session;
    surfxr::DecoderBackend backend=surfxr::DecoderBackend::V4l2;
    bool audio=true;
    bool input=true;
    bool start_vr=false;
    bool adaptive_prediction=true;
    bool available_cpu_affinity=false;
    bool microphone_override=false;
    bool test_head_motion=false;
    std::string layout_path;
    std::string settings_path;
    std::string status_path;
    std::string image_quality_path;
    std::string input_preferences_path;
};
ViewerOptions options(int, char**);
unsigned restore_settings(surfxr::RemoteSettings&,const surfxr::SettingsPreferences&);
surfxr::SettingsPreferences prepare_initial_settings(surfxr::RemoteSettings&);
std::string quoted_error(const std::string&);
std::string decoder_json(const surfxr::DecoderStats&);
std::string xr_json(const surfxr::XrDisplayStats&);
std::string haptics_json(const surfxr::HapticsStats&);
std::string audio_json(const surfxr::AudioStats&);
std::string microphone_json(const surfxr::MicrophoneStats&);
int viewer_self_test();
extern std::atomic_bool stop_requested;
}
