#pragma once
// Private stateful HEVC device owner; all methods run on the decoder producer.
#include <cstdint>
#include <array>
#include <memory>
#include <functional>
#include "surfxr/nv12_frame.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace surfxr {
class V4l2Hevc {
public:
    V4l2Hevc(unsigned width,unsigned height,bool packed_stereo=false);
    ~V4l2Hevc();
    V4l2Hevc(const V4l2Hevc&)=delete;
    V4l2Hevc& operator=(const V4l2Hevc&)=delete;
    int send(const AVPacket* packet);
    // A consumer copies synchronously or acquires a bounded CAPTURE lease.
    // Leased buffers are requeued by this producer after all readers retire.
    // Without a consumer retain the existing independently copied NV12 path.
    int receive(AVFrame* frame,const std::function<void()>& consume={});
    void wait();
    // Valid only on the producer while this owner is alive. After CAPTURE
    // starts, device and lease notifications permit indefinite idle waiting.
    std::array<int,2> readiness_fds()const;
    bool has_pending_work()const;
    // Explicit hardware diagnostics only. Export and immediately close one
    // allocated CAPTURE DMA-BUF; does not retain, import or requeue the buffer.
    int probe_capture_dmabuf_export();
    std::shared_ptr<Nv12FrameStorage> import_capture(const Nv12FrameAllocator& allocator);
    std::shared_ptr<void> retain_capture();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
