#pragma once
#include "network.hpp"
#include "capture.hpp"
#include "surfxr/surfxr_desktop_layout.hpp"
#include <condition_variable>

namespace surfxr::transport::desktop {
struct State {
    std::mutex mutex;std::condition_variable ready;
    Layout layout;bool initialized=false,audio_initialized=false,audio_available=false;
    std::exception_ptr error;
    std::atomic_bool stop{false},stream_ready{false},idr{true};
    std::atomic_uint requested_count{1},capture_requested{15};
    std::atomic_uint capture_applied{15},capture_generation{0};
};
void video_worker(const Options&,Control&,Socket,sockaddr_in,const Key&,State&,bool multi);
}
