#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace surfxr {
inline constexpr std::size_t kMicrophonePacketBytes=1920;
struct MicrophoneStats {
    bool initialized=false,failed=false,stopped=false,source_validated=false,source_is_monitor=false;
    int pulse_error=0;
    std::uint64_t capture_frames=0,consumed_frames=0,nonzero_samples=0,sample_square_sum=0;
    std::uint64_t dropped_old_frames=0,dropped_contention_frames=0,hole_frames=0,invalid_chunks=0;
    std::uint64_t read_callbacks=0;
    std::uint32_t peak=0,queued_frames=0,max_queued_frames=0,server_buffer_bytes=0,server_fragment_bytes=0;
};
// Source names on Frame can contain "alsa_loopback_device" for real microphones.
// Validate Pulse source semantics, never guess from that name substring.
bool microphone_source_allowed(bool monitor_of_sink_valid,const char* media_class,const char* device_class) noexcept;

// Mono S16LE queue. One producer, one consumer; neither waits for a mutex. The
// oldest complete samples are discarded on overflow. Reads return a complete
// requested chunk or zero. stop logically discards all queued and partial data.
class MicrophonePcmRing {
public:
    explicit MicrophonePcmRing(std::uint32_t capacity_frames=2880);
    bool submit(const std::uint8_t* data,std::size_t bytes) noexcept;
    std::size_t read(std::uint8_t* output,std::size_t bytes) noexcept;
    void stop() noexcept;
    MicrophoneStats statistics() const noexcept;
private:
    std::array<std::uint8_t,9600> data_{};
    std::uint8_t partial_=0;
    std::size_t partial_size_=0,head_=0,size_=0,capacity_=0;
    std::mutex mutex_;
    std::atomic_bool stopped_{false};
    std::atomic<std::uint64_t> captured_{0},consumed_{0},nonzero_{0},squares_{0},dropped_old_{0},dropped_busy_{0},invalid_{0};
    std::atomic<std::uint32_t> peak_{0},queued_{0},maximum_{0};
};
class MicrophoneCaptureError : public std::runtime_error {
public:
    MicrophoneCaptureError(const char* stage,int code=0):std::runtime_error(stage),pulse_code(code){}
    int pulse_code;
};
class MicrophoneCapture {
public:
    MicrophoneCapture(); // No Pulse connection and no recording.
    ~MicrophoneCapture();
    MicrophoneCapture(const MicrophoneCapture&)=delete;
    MicrophoneCapture& operator=(const MicrophoneCapture&)=delete;
    // Startup readiness has a shared 3-second deadline. One owner calls start,
    // read_pcm and stop; diagnostics may be read from another thread.
    void start();
    // Nonblocking; returns 0 or 1920 bytes. Throws a fixed diagnostic on failure
    // or two seconds without capture callbacks (silence samples are healthy).
    std::size_t read_pcm(std::uint8_t* output,std::size_t capacity);
    void stop() noexcept;
    MicrophoneStats statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
