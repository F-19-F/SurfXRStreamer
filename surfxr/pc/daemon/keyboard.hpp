#pragma once
#include "../desktop/input.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
#include <functional>

namespace surfxr::daemon {
using CandidateUpdate=void (*)(const std::uint8_t*,std::size_t,bool);
// Reuses desktop::event and desktop::Input, with the same authenticated ticket
// policy. Capture and SendInput stay off both the control and encoding threads.
class Keyboard {
public:
    using Publish=std::function<void(const std::uint8_t*,std::size_t,bool)>;
    explicit Keyboard(Publish,bool clickable=false,bool candidates_only=false);
    ~Keyboard();
    void visible(bool);
    bool input(const surfxr::transport::Bytes&); // recognizes the existing desktop input message
private:
    Publish update_;bool clickable_,candidates_only_;std::mutex mutex_;std::condition_variable ready_;
    std::deque<surfxr::transport::desktop::Event> pending_;bool stop_=false,visible_=false;
    std::thread thread_;
    void run();
};
}
