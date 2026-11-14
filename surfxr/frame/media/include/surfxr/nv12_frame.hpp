#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace surfxr {
// Copied planes or an imported immutable CAPTURE allocation. Imported pixels
// remain owned by their backing allocation and a VideoFrame capture lease.
struct Nv12FrameStorage {
    unsigned width=0,height=0;
    std::array<std::size_t,2> stride{},bytes{};
    virtual ~Nv12FrameStorage()=default;
    virtual std::uint8_t* data(unsigned plane)noexcept=0;
    const std::uint8_t* data(unsigned plane)const noexcept{return const_cast<Nv12FrameStorage*>(this)->data(plane);}
};
struct Nv12DmaBuf {
    int fd=-1;
    unsigned width=0,height=0;
    std::size_t size=0,row_pitch=0,y_offset=0,uv_offset=0;
    std::uint8_t* mapping=nullptr;
    std::shared_ptr<void> backing;
};
struct Nv12FrameAllocator:std::function<std::shared_ptr<Nv12FrameStorage>(unsigned,unsigned)> {
    using Base=std::function<std::shared_ptr<Nv12FrameStorage>(unsigned,unsigned)>;
    using Base::Base;
    std::function<std::shared_ptr<Nv12FrameStorage>(const Nv12DmaBuf&)> import;
};
inline bool valid_nv12_storage(const Nv12FrameStorage& s)noexcept{
    return s.width&&s.height&&s.width<=8192&&s.height<=8192&&s.width%2==0&&s.height%2==0&&
        s.stride[0]>=s.width&&s.stride[1]>=s.width&&s.bytes[0]<=128u*1024u*1024u&&s.bytes[1]<=64u*1024u*1024u&&
        s.stride[0]<=s.bytes[0]/s.height&&s.stride[1]<=s.bytes[1]/(s.height/2);
}
// Value copies own ordinary NV12 bytes, never a writable alias to GPU storage.
class CpuNv12Storage final:public Nv12FrameStorage {
    std::array<std::vector<std::uint8_t>,2> planes_;
public:
    CpuNv12Storage(unsigned w,unsigned h){width=w;height=h;stride={w,w};bytes={std::size_t{w}*h,std::size_t{w}*(h/2)};
        for(unsigned p=0;p<2;++p)planes_[p].resize(bytes[p]);}
    std::uint8_t* data(unsigned p)noexcept override{return planes_[p].data();}
};
inline std::shared_ptr<Nv12FrameStorage> copy_nv12(const Nv12FrameStorage& source,unsigned w,unsigned h,unsigned x=0,unsigned y=0){
    if(!valid_nv12_storage(source)||!w||!h||w%2||h%2||x%2||y%2||x>source.width||w>source.width-x||y>source.height||h>source.height-y||!source.data(0)||!source.data(1))
        throw std::invalid_argument("invalid_nv12_value_copy");
    auto result=std::make_shared<CpuNv12Storage>(w,h);
    for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?h/2:h);++row)
        std::memcpy(result->data(p)+std::size_t{row}*w,
            source.data(p)+std::size_t{row+(p?y/2:y)}*source.stride[p]+x,w);
    return result;
}
}
