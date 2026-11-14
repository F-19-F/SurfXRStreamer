#pragma once
#include "surfxr/nv12_frame.hpp"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace surfxr {
struct VrVideoMetadata;
// Optional CPU-writable RGBA allocation supplied by a renderer. A concrete
// owner may also expose an upload resource, without coupling decoding to a GPU
// API. Returning no storage from an allocator selects ordinary vector memory.
struct RgbaFrameStorage {
    virtual ~RgbaFrameStorage()=default;
    virtual std::uint8_t* data() noexcept=0;
    virtual std::size_t size() const noexcept=0;
};
using RgbaFrameAllocator=std::function<std::shared_ptr<RgbaFrameStorage>(std::size_t)>;
// Immutable after publication. RGBA8 or optional NV12 planes, no HDR tone mapping. The source
// timestamp retains the server's units; it is NOT an OpenXR time value.
struct VideoFrame {
    std::uint32_t width=0,height=0,stride=0;
    std::uint64_t generation=0,source_timestamp=0,decoded_monotonic_ns=0;
    // Nonzero only for demand-gated output. Consumers reject a frame whose
    // epoch no longer matches their current render demand, under their lock.
    std::uint64_t output_epoch=0;
    // Local monotonic diagnostics. Submit time follows the exact access unit
    // through decoder reordering; conversion starts just before RGBA scaling.
    // Zero submit time means no matching source stamp was available.
    std::uint64_t packet_submit_monotonic_ns=0,conversion_begin_monotonic_ns=0;
    // Codec output acquired, before CPU color conversion. Unlike decoded_ns,
    // this endpoint separates codec latency from conversion and frame waiting.
    std::uint64_t decoder_output_monotonic_ns=0;
    bool has_source_timestamp=false;
    // Travels with the encoded access unit through decoder reordering. Never
    // attach the most recently received pose to an older decoded VR picture.
    std::shared_ptr<const VrVideoMetadata> vr_metadata;
    std::vector<std::uint8_t> rgba;
    // Consumers may inspect the storage identity, but pixels stay immutable
    // after publication. GPU readers must retain the whole VideoFrame until
    // their read completes: the decoder pool checks frame ownership for reuse.
    std::shared_ptr<RgbaFrameStorage> rgba_storage;
    std::shared_ptr<Nv12FrameStorage> nv12_storage;
    // Released only after pairing, surfaces and the final GPU fence retire.
    // The decoder producer alone observes release and requeues CAPTURE.
    std::shared_ptr<void> capture_lease;
    // Kr, Kb, luma offset, luma scale. Zero Kr denotes the ordinary RGBA path.
    std::array<float,4> yuv_coefficients{};
    // SURFXR eye view into one immutable packed allocation. This owner, rather
    // than a second writable storage alias, keeps the decoder pool entry busy
    // through pairing history and the GPU fence. Ordinary frames stay unchanged.
    std::shared_ptr<const VideoFrame> packed_source;
    unsigned packed_eye=0;
    bool packed_region=false;
    unsigned packed_x=0,packed_y=0;
    unsigned source_x()const noexcept{return packed_source?(packed_region?packed_x:packed_eye*width):0;}
    unsigned source_y()const noexcept{return packed_source&&packed_region?packed_y:0;}
    bool valid_packed_region()const noexcept{
        if(!packed_source)return true;
        const auto& p=*packed_source;const auto x=source_x(),y=source_y();
        return !p.packed_source&&x<=p.width&&width<=p.width-x&&y<=p.height&&height<=p.height-y&&
            (packed_region?(x%2==0&&y%2==0&&width%2==0&&height%2==0):(packed_eye<=1&&p.width==2*width&&p.height==height));
    }
    std::array<float,4> source_rect()const noexcept{
        if(!packed_source)return {0,0,1,1};
        const auto& p=*packed_source;return {float(source_x())/p.width,float(source_y())/p.height,float(width)/p.width,float(height)/p.height};
    }
    std::uint8_t* rgba_data() noexcept{return rgba_storage?rgba_storage->data():rgba.data();}
    const std::uint8_t* rgba_data()const noexcept{return rgba_storage?rgba_storage->data():rgba.data();}
    std::size_t rgba_size()const noexcept{return rgba_storage?rgba_storage->size():rgba.size();}
    std::size_t rgba_capacity()const noexcept{return rgba_storage?rgba_storage->size():rgba.capacity();}
    VideoFrame()=default;
    // Preserve the original value semantics: copying a frame never creates a
    // second writable alias to externally owned pixels or an upload handle.
    VideoFrame(const VideoFrame& other):width(other.width),height(other.height),stride(other.stride),
        generation(other.generation),source_timestamp(other.source_timestamp),decoded_monotonic_ns(other.decoded_monotonic_ns),output_epoch(other.output_epoch),
        packet_submit_monotonic_ns(other.packet_submit_monotonic_ns),conversion_begin_monotonic_ns(other.conversion_begin_monotonic_ns),
        decoder_output_monotonic_ns(other.decoder_output_monotonic_ns),
        has_source_timestamp(other.has_source_timestamp),vr_metadata(other.vr_metadata),yuv_coefficients(other.yuv_coefficients){
        const auto& source=other.packed_source?*other.packed_source:other;
        if(source.nv12_storage){nv12_storage=copy_nv12(*source.nv12_storage,width,height,other.source_x(),other.source_y());}
        else if(other.packed_source){
            stride=width*4;rgba.resize(std::size_t{stride}*height);
            const auto& source=*other.packed_source;
            for(unsigned row=0;row<height;++row)
                std::copy_n(source.rgba_data()+std::size_t{row+other.source_y()}*source.stride+other.source_x()*4,
                    stride,rgba.data()+std::size_t{row}*stride);
        }else if(other.rgba_size())rgba.assign(other.rgba_data(),other.rgba_data()+other.rgba_size());
    }
    VideoFrame& operator=(const VideoFrame& other){if(this!=&other){VideoFrame copy(other);*this=std::move(copy);}return *this;}
    VideoFrame(VideoFrame&&) noexcept=default;
    VideoFrame& operator=(VideoFrame&&) noexcept=default;
};
// Exactly one published frame. Readers hold at most one snapshot and release
// the mutex before GPU work. Publishing never waits for XR rendering/events.
class LatestFrameSlot {
public:
    void publish(std::shared_ptr<const VideoFrame> frame) {
        std::lock_guard<std::mutex> lock(mutex_);frame_=std::move(frame);
    }
    std::shared_ptr<const VideoFrame> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);return frame_;
    }
    void clear() {std::lock_guard<std::mutex> lock(mutex_);frame_.reset();}
private:
    mutable std::mutex mutex_;
    std::shared_ptr<const VideoFrame> frame_;
};
} // namespace surfxr
