#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace surfxr::nv12_test {
// Benchmark candidates only: repeated live A/B did not establish a gain.
// Exact, non-overlapping plane bytes. Small/unaligned copies keep libc's path.
// The ARM64 streaming stores are a cache hint, not a change to ownership or
// host/GPU synchronization. No access extends beyond the caller's byte span.
template<bool Streaming,bool Prefetch=false> inline void copy_nv12_plane_arm64(std::uint8_t* target,const std::uint8_t* source,std::size_t bytes){
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    if(bytes>=64u*1024u&&!(reinterpret_cast<std::uintptr_t>(target)&63)&&
       !(reinterpret_cast<std::uintptr_t>(source)&15)){
        while(bytes>=128){
            if constexpr(Prefetch)if(bytes>512)
                asm volatile("prfm pldl1strm, [%[s], #512]" : : [s] "r" (source));
            if constexpr(Streaming){
                asm volatile(
                    "ldp q0, q1, [%[s]]\n"
                    "ldp q2, q3, [%[s], #32]\n"
                    "ldp q4, q5, [%[s], #64]\n"
                    "ldp q6, q7, [%[s], #96]\n"
                    "stnp q0, q1, [%[d]]\n"
                    "stnp q2, q3, [%[d], #32]\n"
                    "stnp q4, q5, [%[d], #64]\n"
                    "stnp q6, q7, [%[d], #96]\n"
                    : : [s] "r" (source),[d] "r" (target)
                    : "v0","v1","v2","v3","v4","v5","v6","v7","memory");
            }else{
                asm volatile(
                    "ldp q0, q1, [%[s]]\n"
                    "ldp q2, q3, [%[s], #32]\n"
                    "ldp q4, q5, [%[s], #64]\n"
                    "ldp q6, q7, [%[s], #96]\n"
                    "stp q0, q1, [%[d]]\n"
                    "stp q2, q3, [%[d], #32]\n"
                    "stp q4, q5, [%[d], #64]\n"
                    "stp q6, q7, [%[d], #96]\n"
                    : : [s] "r" (source),[d] "r" (target)
                    : "v0","v1","v2","v3","v4","v5","v6","v7","memory");
            }
            source+=128;target+=128;bytes-=128;
        }
    }
#endif
    if(bytes)std::memcpy(target,source,bytes);
}
} // namespace surfxr::nv12_test
