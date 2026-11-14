#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

// Registration succeeds before SteamVR necessarily finishes Activate(). A timeout
// must retain the registered device; update callbacks wait for published readiness.
class DeviceActivation {
public:
    bool wait(std::chrono::milliseconds timeout) {
        auto lock = std::unique_lock<std::mutex>(mutex_);
        condvar_.wait_for(lock, timeout, [this] {
            return state_.load(std::memory_order_acquire) != State::Pending;
        });
        return state_.load(std::memory_order_acquire) != State::Failed;
    }

    void complete(bool success) {
        {
            auto lock = std::lock_guard<std::mutex>(mutex_);
            state_.store(success ? State::Ready : State::Failed, std::memory_order_release);
        }
        condvar_.notify_all();
    }

    bool ready() const { return state_.load(std::memory_order_acquire) == State::Ready; }

    void deactivate() { state_.store(State::Pending, std::memory_order_release); }

private:
    enum class State { Pending, Ready, Failed };
    std::atomic<State> state_ { State::Pending };
    std::mutex mutex_;
    std::condition_variable condvar_;
};
