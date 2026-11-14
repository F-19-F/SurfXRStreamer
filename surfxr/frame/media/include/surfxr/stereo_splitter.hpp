#pragma once
#include "surfxr/latest_frame.hpp"
#include <array>

namespace surfxr {
// Receive-decoder owner only. Six immutable frames per eye bound memory and
// protect mapped GPU source allocations until XR releases its frame references.
class StereoSplitter {
public:
    std::array<std::shared_ptr<const VideoFrame>,4> split_gaze(std::shared_ptr<const VideoFrame> source);
    explicit StereoSplitter(RgbaFrameAllocator allocator={}):allocator_(std::move(allocator)){}
    std::array<std::shared_ptr<const VideoFrame>,2> split(const VideoFrame&);
    // Owning views avoid copying a GPU-readable packed source. The const-ref
    // overload retains the CPU fallback and original value semantics.
    std::array<std::shared_ptr<const VideoFrame>,2> split(std::shared_ptr<const VideoFrame>);
private:
    RgbaFrameAllocator allocator_;
    // Three entries can remain in pairing history; XR can retain older GPU
    // sources. Extra slots let publication retire that history without reuse.
    std::array<std::array<std::shared_ptr<VideoFrame>,6>,2> pool_{};
};
}
