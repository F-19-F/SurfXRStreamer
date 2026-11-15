#pragma once
#include "connection.hpp"
#include <future>

namespace surfxr::launcher {
// Main-thread snapshot owner. Probe work never runs on the IPC thread.
class NetworkMonitor {
 public:
  using Probe = std::function<std::future<Json>()>;
  explicit NetworkMonitor(Probe probe = [] {
    return std::async(std::launch::async, local_network);
  }) : probe_(std::move(probe)) {}
  void poll(Time now = Clock::now()) {
    if (pending_.valid()) {
      if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      try {
        snapshot_ = pending_.get();
        next_ = now + std::chrono::seconds(5);
      } catch (const std::exception&) {
        // A failed sample is not evidence that an interface disappeared.
        next_ = now + std::chrono::seconds(1);
      }
    }
    if (now >= next_) {
      try { pending_ = probe_(); }
      catch (const std::exception&) { next_ = now + std::chrono::seconds(1); }
    }
  }
  Json snapshot() const { return snapshot_; }
 private:
  Probe probe_;
  std::future<Json> pending_;
  Time next_{};
  Json snapshot_ = object({{"interfaces", Json::array()}, {"routes", Json::array()}, {"neighbors", Json::array()}});
};
}
