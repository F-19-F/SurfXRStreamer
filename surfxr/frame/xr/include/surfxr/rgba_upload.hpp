#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace surfxr {
struct RgbaUploadEdgeRegion {
    std::size_t offset=0; // tightly packed RGBA byte offset in the edge buffer
    unsigned x=0,y=0,width=0,height=0;
};
struct RgbaUploadEdgePlan {
    std::array<RgbaUploadEdgeRegion,4> regions{};
    std::size_t bytes=0;
    bool valid()const noexcept{return bytes!=0;}
};
// Four disjoint strips complement the unchanged [2,w-2) x [2,h-2)
// interior. Small images have no such interior and use rgba_upload_copy.
inline RgbaUploadEdgePlan rgba_upload_edge_plan(unsigned width,unsigned height){
    RgbaUploadEdgePlan result;if(width<=4||height<=4)return result;
    result.regions={{{0,0,0,width,2},{0,0,height-2,width,2},
                     {0,0,2,2,height-4},{0,width-2,2,2,height-4}}};
    for(auto& region:result.regions){region.offset=result.bytes;result.bytes+=std::size_t{region.width}*region.height*4;}
    return result;
}
// The source points at the top-left of the validated crop, which may retain
// the full decoded row stride. Preserve arbitrary alpha and never mutate the
// immutable frame or read write-combined destination memory. Vulkan can copy
// the remaining interior directly from that frame's mapped buffer.
inline RgbaUploadEdgePlan rgba_upload_edge_copy(std::uint8_t* destination,const std::uint8_t* source,
                                                std::size_t source_stride,unsigned width,unsigned height){
    const auto plan=rgba_upload_edge_plan(width,height);
    for(const auto& region:plan.regions)for(unsigned row=0;row<region.height;++row)for(unsigned col=0;col<region.width;++col){
        const unsigned x=region.x+col,y=region.y+row;
        const auto* src=source+std::size_t{y}*source_stride+std::size_t{x}*4;
        auto* dst=destination+region.offset+(std::size_t{row}*region.width+col)*4;
        const auto edge=std::min({x,y,width-1-x,height-1-y});
        const auto alpha=static_cast<std::uint8_t>((unsigned(src[3])*(edge?192u:64u)+127u)/255u);
        const std::uint8_t pixel[]{src[0],src[1],src[2],alpha};std::memcpy(dst,pixel,4);
    }
    return plan;
}
// The caller validates the source/crop and provides width*height*4 destination
// bytes. The destination can be write-combined Vulkan memory: never read it.
// In particular, alpha feathering and channel swizzling read immutable normal
// CPU source memory rather than doing byte read/modify/write on mapped memory.
inline void rgba_upload_copy(std::uint8_t* destination,const std::uint8_t* source,
                             std::size_t source_stride,unsigned width,unsigned height,
                             bool bgra,bool feather) {
    const std::size_t stride=std::size_t{width}*4;
    if(!bgra&&!feather&&source_stride==stride){std::memcpy(destination,source,stride*height);return;}
    for(unsigned row=0;row<height;++row){
        const auto* src=source+std::size_t{row}*source_stride;
        auto* dst=destination+std::size_t{row}*stride;
        if(!bgra)std::memcpy(dst,src,stride);
        else{
            unsigned col=0;
#if defined(__aarch64__)
            for(;col+16<=width;col+=16){
                auto channels=vld4q_u8(src+std::size_t{col}*4);
                const auto red=channels.val[0];channels.val[0]=channels.val[2];channels.val[2]=red;
                vst4q_u8(dst+std::size_t{col}*4,channels);
            }
#endif
            for(;col<width;++col){
                const auto at=std::size_t{col}*4;
                const std::uint8_t pixel[]{src[at+2],src[at+1],src[at],src[at+3]};
                std::memcpy(dst+at,pixel,4);
            }
        }
        if(!feather)continue;
        const auto edge_pixel=[&](unsigned col){
            const auto edge=std::min({col,row,width-1-col,height-1-row});
            if(edge>=2)return;
            const auto at=std::size_t{col}*4;
            const auto alpha=static_cast<std::uint8_t>((unsigned(src[at+3])*(edge?192u:64u)+127u)/255u);
            const std::uint8_t pixel[]{src[at+(bgra?2:0)],src[at+1],src[at+(bgra?0:2)],alpha};
            std::memcpy(dst+at,pixel,4);
        };
        if(row<2||row+2>=height)for(unsigned col=0;col<width;++col)edge_pixel(col);
        else{
            for(unsigned col=0;col<std::min(width,2u);++col)edge_pixel(col);
            for(unsigned col=std::max(2u,width>2?width-2:0u);col<width;++col)edge_pixel(col);
        }
    }
}
} // namespace surfxr
