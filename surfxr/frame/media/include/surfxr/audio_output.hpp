#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace surfxr {
// Buffered adds 12 ms of reserve and 2 ms transitions for multiplexed SURFXR.
enum class AudioPlaybackMode {Immediate, Buffered};
struct AudioStats {
    bool initialized=false,failed=false,stopped=false;
    // initialized is historical; ready describes the current connection.
    bool ready=false,reconnecting=false;
    int pulse_error=0;
    int context_state=0,stream_state=0;
    std::uint64_t connection_attempts=0,successful_connections=0,reconnects=0,connection_failures=0,queue_resets=0;
    std::uint64_t submitted_bytes=0,pcm_frames_consumed=0,silence_frames=0;
    // silence_frames counts missing PCM, including tapered concealment.
    std::uint64_t dropped_old_frames=0,dropped_contention_frames=0;
    std::uint64_t underrun_callbacks=0,server_underflows=0,invalid_submissions=0;
    std::uint64_t submissions_after_stop=0,write_callbacks=0,written_frames=0;
    std::uint64_t rebuffer_callbacks=0,rebuffer_events=0,transition_frames=0;
    std::uint32_t queued_frames=0,max_queued_frames=0,partial_bytes=0;
    std::uint32_t buffering_target_frames=0;
    std::uint32_t server_buffer_bytes=0,server_target_bytes=0;
};

// Implementation-independent PCM queue, exposed for offline invariant tests.
// Exactly one producer calls submit; exactly one consumer calls read. Both use
// Immediate uses try_lock; Buffered uses atomic, overwrite-safe PCM slots.
// stop is safe concurrently; destruction requires callers joined.
// Frames are four bytes: interleaved S16LE, FL/FR, 48000 Hz. Partial frames stay
// on the producer side and are never emitted as half a stereo sample.
class AudioPcmRing {
public:
    explicit AudioPcmRing(std::uint32_t capacity_frames=2880,AudioPlaybackMode mode=AudioPlaybackMode::Immediate); // 60 ms, max 100 ms
    bool submit(const std::uint8_t* data,std::size_t size) noexcept;
    // Returns consumed PCM bytes; Buffered may also emit a taper in the tail.
    std::size_t read(std::uint8_t* output,std::size_t aligned_bytes) noexcept;
    // Discard a disconnected connection's PCM. Concurrent submit/read remain
    // safe; a submit crossing this boundary cannot enqueue its older samples.
    void discard_pending() noexcept;
    void stop() noexcept;
    AudioStats statistics() const noexcept;
private:
    friend class AudioOutput;
    friend unsigned audio_output_recovery_self_test();
    bool submit_in_epoch(const std::uint8_t* data,std::size_t size,std::uint64_t epoch) noexcept;
    void smooth(std::uint8_t* output,std::size_t bytes,std::size_t actual,bool discontinuity) noexcept;
    bool submit_buffered(const std::uint8_t*,const std::array<std::uint8_t,4>&,std::size_t,std::size_t,std::uint64_t) noexcept;
    std::size_t read_buffered(std::uint8_t*,std::size_t) noexcept;
    std::uint64_t discard_buffered(std::uint64_t floor) noexcept;
    struct BufferedSlot {std::atomic<std::uint64_t> stamp{0},pcm_epoch{0};};
    std::array<BufferedSlot,4800> buffered_data_{};
    std::atomic<std::uint64_t> buffered_written_{0},buffered_read_{0};
    std::array<std::uint8_t,4800*4> data_{};
    std::array<std::uint8_t,4> partial_{};
    std::size_t partial_size_=0,head_=0,size_=0,capacity_=0;
    std::mutex mutex_;
    std::atomic_bool stopped_{false};
    std::atomic<std::uint64_t> submitted_{0},consumed_{0},silence_{0},dropped_old_{0},dropped_busy_{0};
    std::atomic<std::uint64_t> underruns_{0},invalid_{0},after_stop_{0};
    std::atomic<std::uint32_t> queued_{0},maximum_{0},partial_count_{0};
    std::atomic<std::uint64_t> reset_epoch_{0},partial_epoch_{0},resets_{0};
    std::uint64_t producer_epoch_=0;
    const bool buffered_;
    // Consumer-owned state; reset is observed through reset_epoch_.
    bool primed_=false;
    unsigned fade_remaining_=0;
    std::array<std::int32_t,2> last_sample_{},fade_from_{};
    std::uint64_t consumer_epoch_=0,consumer_dropped_=0;
    std::atomic<std::uint64_t> rebuffer_callbacks_{0},rebuffer_events_{0},transitions_{0};
};

class AudioOutputError : public std::runtime_error {
public:
    AudioOutputError(const char* stage,int code=0):std::runtime_error(stage),pulse_code(code){}
    int pulse_code;
};

class AudioOutput {
public:
    explicit AudioOutput(AudioPlaybackMode mode=AudioPlaybackMode::Immediate); // No server connection or sound in the constructor.
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    // Explicit playback action; call only after the viewer's audio enable flag.
    // Starts a background connection owner and returns immediately. Each
    // attempt has a 3 s readiness deadline, with 250 ms..2 s retry backoff.
    // initialize/stop run on one caller thread; stop wakes and joins the owner.
    void initialize();
    // One network producer; never waits for PulseAudio, queue space, or playback.
    bool submit_pcm(const std::uint8_t* data,std::size_t size) noexcept;
    AudioStats statistics() const noexcept;
    // Disconnects immediately, without draining queued sound; idempotent.
    void stop() noexcept;
private:
    friend unsigned audio_output_recovery_self_test();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Offline fake-connection lifecycle checks; never connects to a sound server.
unsigned audio_output_recovery_self_test();
} // namespace surfxr
