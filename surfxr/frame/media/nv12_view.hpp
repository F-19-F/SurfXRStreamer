#pragma once
// Decoder-producer-only borrowed MMAP view. No reference may escape consume().
#include "surfxr/video_decoder.hpp"
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}
#include <functional>

namespace surfxr::decoder_detail {
inline void consume_nv12_view(AVFrame& frame,std::uint8_t* data,std::size_t bytes,
                             unsigned stride,std::size_t uv,const std::function<void()>& consume){
    const auto invalid=frame.format!=AV_PIX_FMT_NV12||frame.width<=0||frame.height<=0||
        frame.buf[0]||!data||!consume||stride<static_cast<unsigned>(frame.width)||
        uv<std::size_t(stride)*frame.height||uv>bytes||
        std::size_t(stride)*((frame.height+1)/2)>bytes-uv;
    if(invalid)throw VideoDecodeError("hevc_borrowed_layout_invalid");
    // sws_scale_frame requires a refcounted source. This reference borrows
    // the mapping only for its synchronous workers; it neither unmaps nor
    // queues the device buffer. The device owner queues it after this returns.
    frame.buf[0]=av_buffer_create(data,bytes,[](void*,std::uint8_t*){},nullptr,AV_BUFFER_FLAG_READONLY);
    if(!frame.buf[0])throw VideoDecodeError("hevc_borrowed_reference_failed");
    struct Cleanup {AVFrame& frame;~Cleanup(){av_frame_unref(&frame);}} cleanup{frame};
    frame.data[0]=data;frame.data[1]=data+uv;
    frame.linesize[0]=frame.linesize[1]=static_cast<int>(stride);
    consume();
}
} // namespace surfxr::decoder_detail
