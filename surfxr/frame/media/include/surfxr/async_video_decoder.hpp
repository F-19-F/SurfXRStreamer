#pragma once
#include "surfxr/video_decoder.hpp"

namespace surfxr {
// One dedicated producer thread owns each decoder. Encoded packets stay in
// order; the bounded queue applies backpressure without dropping references.
// Public commands have one owner (the receiver); statistics/check are safe
// from another thread. Sinks must not reenter this wrapper or its decoder.
class AsyncVideoDecoder {
public:
    using FrameSink=VideoDecoder::FrameSink;
    using FrameDemand=VideoDecoder::FrameDemand;
    explicit AsyncVideoDecoder(LatestFrameSlot& output,DecoderBackend backend=DecoderBackend::Software,
                               FrameSink sink={},unsigned rgba_threads=1,RgbaFrameAllocator allocator={},FrameDemand demand={},
                               DecoderFrameLayout layout=DecoderFrameLayout::Single,Nv12FrameAllocator nv12_allocator={});
    ~AsyncVideoDecoder();
    AsyncVideoDecoder(const AsyncVideoDecoder&)=delete;
    AsyncVideoDecoder& operator=(const AsyncVideoDecoder&)=delete;
    // Both commands discard old queued packets and wait for any current
    // decode/callback. Configure always starts a new reference-frame chain,
    // including repeated dimensions, because discarded packets cannot be used
    // as references. The restart argument is retained for API compatibility.
    void configure(std::uint32_t codec,std::uint32_t width,std::uint32_t height,bool restart=false);
    void deactivate();
    void submit(const std::uint8_t* data,std::size_t size,std::uint64_t source_timestamp,
                std::shared_ptr<const VrVideoMetadata> vr_metadata={});
    // Takes the complete encoded owner without copying. Bytes before offset
    // are retained but not decoded; the full owner counts against byte limits.
    void submit(std::vector<std::uint8_t>&& bytes,std::size_t offset,std::uint64_t source_timestamp,
                std::shared_ptr<const VrVideoMetadata> vr_metadata={});
    // Rejects a full queue immediately. The caller must discard predicted
    // packets until a new IDR, preserving the accepted reference chain.
    bool try_submit(const std::uint8_t* data,std::size_t size,std::uint64_t source_timestamp,
                    std::shared_ptr<const VrVideoMetadata> vr_metadata={});
    bool try_submit(std::vector<std::uint8_t>&& bytes,std::size_t offset,std::uint64_t source_timestamp,
                    std::shared_ptr<const VrVideoMetadata> vr_metadata={});
    DecoderStats statistics() const;
    bool needs_keyframe() const;
    void keyframe_request_queued();
    void check() const; // Propagate the original background decode/sink failure.
    // Discard pending packets, finish any current callback and join. Call
    // before destroying callback captures, output slots or the Vulkan device.
    // This does not send EOS or flush old output. Statistics remain readable.
    void stop() noexcept;
private:
    struct Impl;
    explicit AsyncVideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend int async_video_decoder_self_test();
};
int async_video_decoder_self_test();
} // namespace surfxr
