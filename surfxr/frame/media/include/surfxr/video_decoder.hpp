#pragma once
#include "surfxr/latest_frame.hpp"
#include <cstddef>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace surfxr {
enum class DecoderBackend { Software, V4l2 };
enum class DecoderFrameLayout { Single, PackedStereo };
const char* decoder_backend_name(DecoderBackend backend) noexcept;
class VideoDecodeError : public std::runtime_error {
public:
    int ffmpeg_code;
    explicit VideoDecodeError(const char* stage,int code=0):std::runtime_error(stage),ffmpeg_code(code){}
};
struct DecoderStats {
    std::uint64_t packets=0,frames=0,configuration_changes=0,decode_time_ns=0;
    // decode_time_ns includes packet submission, decoding and RGBA conversion;
    // this subset measures colorspace setup, allocation and the RGBA copy.
    std::uint64_t rgba_conversion_time_ns=0;
    // frames counts valid codec outputs, including gated outputs. Converted
    // counts completed swscale calls; suppressed is a subset of converted.
    std::uint64_t rgba_converted_frames=0,rgba_skipped_frames=0,rgba_suppressed_publications=0;
    std::uint64_t nv12_published_frames=0,nv12_copy_time_ns=0,nv12_pool_bytes=0;
    std::uint64_t nv12_zero_copy_frames=0,nv12_zero_copy_time_ns=0,nv12_zero_copy_fallback_frames=0;
    // Wall time in codec/device send/receive, excluding publish/conversion.
    // HEVC receive covers device/view handling, excluding synchronous RGBA
    // conversion; these are not hardware execution times.
    std::uint64_t avcodec_send_time_ns=0,avcodec_receive_time_ns=0;
    std::uint32_t rgba_conversion_threads=1; // CPU color conversion only; decoding remains selected backend
    // Allocations include temporary buffers when every retained frame is held.
    // The pool owns at most6 frames /128MiB (256MiB for packed stereo); published frames remain immutable
    // while any consumer holds a shared_ptr to them.
    std::uint64_t rgba_buffer_allocations=0,rgba_buffer_reuses=0,rgba_temporary_allocations=0;
    std::uint64_t rgba_pool_buffers=0,rgba_pool_bytes=0;
    // AsyncVideoDecoder overlays these bounded-queue counters on its copied
    // snapshot. Plain synchronous decoders retain zero values.
    std::uint64_t async_queue_packets_max=0,async_queue_bytes_max=0;
    std::uint64_t async_backpressure_wait_ns=0,async_discarded_packets=0;
    std::uint64_t metadata_from_opaque=0,metadata_from_pts=0,metadata_unmatched=0;
    std::uint64_t sequence_changes=0,sequence_restarts=0,waiting_for_keyframe_packets=0;
    std::uint64_t hardware_busy_recoveries=0;
    std::uint32_t width=0,height=0;
    // Numeric AVColorSpace/AVColorRange from the latest decoded AVFrame.
    // -1 means no frame has been observed; retain unspecified values verbatim.
    int color_space=-1,color_range=-1;
    // Matrix/range used by CPU/GPU conversion. Known per-picture SPS fields
    // override V4L2 device metadata; the raw fields above remain unchanged.
    int effective_color_space=-1,effective_color_range=-1;
    DecoderBackend backend=DecoderBackend::Software;
    // Fixed codec/device names with static lifetime; never contain media or identity.
    const char* selected_decoder="none";
    const char* pixel_format="none";
    bool failed=false;
    int ffmpeg_code=0;
};
// A single producer owns this synchronous decoder with CPU-readable output.
// V4l2 explicitly selects H264 or HEVC8 hardware; no software fallback.
// Complete packets go to libavcodec or the owned stateful HEVC device;
// no container/file demuxer is used live. HEVC capture starts at SOURCE_CHANGE,
// converts RGBA or copies NV12 before returning CAPTURE. An explicit GPU NV12
// allocator may instead import CAPTURE with a bounded, fence-protected lease.
// Backpressure is TCP. Changed parameters wait for random access before a
// hardware restart (AVC IDR or HEVC IRAP). AVC starts with validated SPS/PPS.
// HEVC polls are nonblocking and input/drain waits have a two-second deadline.
// H.264 V4l2 FFmpeg calls may block independently of the network stop flag. Callers
// need external process supervision for a hard stop deadline.
class VideoDecoder {
public:
    // Optional synchronous observer of every published RGBA/NV12 frame, including
    // bursts, idle poll and finish. Demand-suppressed codec outputs do not call
    // this observer. Called after slot publish, with original packet metadata.
    // Must not block or reenter decoder methods.
    // Throwing marks this decoder failed and propagates through submit/poll/finish.
    using FrameSink=std::function<void(std::shared_ptr<const VideoFrame>)>;
    // Optional thread-safe, nonblocking demand token. Zero suppresses RGBA;
    // every resume uses a new nonzero epoch. Encoded input is always decoded.
    // Input and conversion epochs must match, and are rechecked after swscale.
    // Consumers must also check VideoFrame::output_epoch under their own state
    // lock, since demand can change immediately after the final decoder check.
    using FrameDemand=std::function<std::uint64_t()>;
    // Hardware video paths may request 4 bounded swscale color-conversion
    // workers. Normal software use retains the default 1; offline tests may
    // request 4. These workers do not change decoding or metadata ordering.
    // Optional storage is requested only for bounded retained pool entries.
    // The allocator/device must outlive this decoder and every published frame.
    // PackedStereo permits 32Mi pixels for two eyes; Single retains 16Mi.
    // An optional NV12 allocator publishes copied planes or leased DMA-BUFs for
    // GPU color conversion in either layout; a missing allocator retains RGBA.
    explicit VideoDecoder(LatestFrameSlot& output,DecoderBackend backend=DecoderBackend::Software,FrameSink sink={},unsigned rgba_threads=1,
                          RgbaFrameAllocator allocator={},FrameDemand demand={},DecoderFrameLayout layout=DecoderFrameLayout::Single,
                          Nv12FrameAllocator nv12_allocator={});
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&)=delete;
    VideoDecoder& operator=(const VideoDecoder&)=delete;
    void configure(std::uint32_t codec,std::uint32_t width,std::uint32_t height,bool restart=false);
    void submit(const std::uint8_t* data,std::size_t size,std::uint64_t source_timestamp,
                std::shared_ptr<const VrVideoMetadata> vr_metadata={},
                std::uint64_t original_submit_monotonic_ns=0,
                std::optional<std::uint64_t> original_output_epoch={});
    // Receive already available output without submitting input, EOS or a
    // flush. Device and FFmpeg's V4L2 idle-input paths poll without waiting;
    // keep calling on the same producer thread between network reads. Returns
    // decoded outputs, including gated outputs; inactive/finished return zero.
    // Driver events retain the external supervision requirement above.
    std::size_t poll();
    // Same producer thread only. HEVC exposes device/lease readiness after
    // SOURCE_CHANGE; legacy FFmpeg hardware uses bounded pending-output polls.
    std::array<int,2> readiness_fds()const;
    bool idle_poll_needed()const;
    // Drop pending output without EOS and release decoder/MMAP resources. The
    // slot is cleared; cumulative statistics and generation remain unchanged.
    // An active decoder can be configured again. Failed/finished stay terminal.
    void deactivate();
    void finish(); // Drain complete packets only; never feed a truncated network packet.
    bool needs_keyframe() const;
    void keyframe_request_queued(); // Clear only after control queue accepts it.
    DecoderStats statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend int video_decoder_self_test();
    friend void nv12_output_self_test();
};
int video_decoder_self_test();
} // namespace surfxr
