#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace surfxr {
// Values match AVColorSpace/AVColorRange, without making the elementary-stream
// inspection headers depend on FFmpeg. A parsed SPS defaults to limited range
// as specified by AVC/HEVC; absent colour_description keeps its matrix unknown.
struct VideoColorInfo {
    int color_space=2;
    int color_range=0;
};

namespace video_color_detail {
constexpr std::size_t max_sps_bytes=4096;
struct Bits {
    const std::uint8_t* data;
    std::size_t size,position=0;
    bool skip(std::size_t count) noexcept {
        if(count>size*8-position)return false;
        position+=count;return true;
    }
    bool get(unsigned count,unsigned& value) noexcept {
        if(count>32||count>size*8-position)return false;
        value=0;
        for(unsigned i=0;i<count;++i){value=(value<<1)|((data[position/8]>>(7-position%8))&1);++position;}
        return true;
    }
    bool ue(unsigned maximum,unsigned& value) noexcept {
        unsigned zeros=0,bit=0;
        do{if(!get(1,bit))return false;if(!bit&&++zeros>30)return false;}while(!bit);
        unsigned suffix=0;if(!get(zeros,suffix))return false;
        value=((1u<<zeros)-1)+suffix;return value<=maximum;
    }
    bool se(int minimum,int maximum,int* output=nullptr) noexcept {
        unsigned code=0;if(!ue(65535,code))return false;
        const int value=(code&1)?static_cast<int>((code+1)/2):-static_cast<int>(code/2);
        if(value<minimum||value>maximum)return false;
        if(output)*output=value;
        return true;
    }
};
inline bool unescape(const std::uint8_t* data,std::size_t size,
                     std::array<std::uint8_t,max_sps_bytes>& output,std::size_t& count) noexcept {
    if(!data||!size||size>output.size())return false;
    unsigned zeros=0;count=0;
    for(std::size_t i=0;i<size;++i){const auto byte=data[i];
        if(zeros>=2&&byte==3){if(i+1==size||data[i+1]>3)return false;zeros=0;continue;}
        if(zeros>=2&&byte<3)return false;
        output[count++]=byte;zeros=byte==0?zeros+1:0;
    }
    return count!=0;
}
inline bool avc_scaling(Bits& bits,unsigned count) noexcept {
    int last=8,next=8;
    for(unsigned i=0;i<count;++i){
        if(next){int delta=0;if(!bits.se(-128,127,&delta))return false;next=(last+delta+256)%256;}
        if(next)last=next;
    }
    return true;
}
// Both standards put the fields needed for YCbCr conversion at the beginning
// of VUI. We deliberately do not interpret timing, HRD, or later extensions.
inline bool vui_color(Bits& bits,VideoColorInfo& color) noexcept {
    unsigned present=0,value=0;
    if(!bits.get(1,present))return false;
    if(present){if(!bits.get(8,value)||(value==255&&!bits.skip(32)))return false;}
    if(!bits.get(1,present)||(present&&!bits.skip(1)))return false;
    if(!bits.get(1,present))return false;
    if(present){
        unsigned full=0,description=0;
        if(!bits.skip(3)||!bits.get(1,full)||!bits.get(1,description))return false;
        color.color_range=full?2:1;
        if(description){
            if(!bits.skip(16)||!bits.get(8,value))return false;
            // H.273 matrix 3 is reserved. Values above the standardized
            // ICtCp entry are unknown to this SDR 4:2:0 decoder path.
            color.color_space=value==3||value>14?2:static_cast<int>(value);
        }
    }
    return true;
}
inline bool hevc_profile(Bits& bits,unsigned layers) noexcept {
    if(!bits.skip(96))return false;
    std::array<unsigned,7> profile{},level{};
    for(unsigned i=0;i<layers;++i)if(!bits.get(1,profile[i])||!bits.get(1,level[i]))return false;
    if(layers&&!bits.skip((8-layers)*2))return false;
    for(unsigned i=0;i<layers;++i)if((profile[i]&&!bits.skip(88))||(level[i]&&!bits.skip(8)))return false;
    return true;
}
inline bool hevc_scaling(Bits& bits) noexcept {
    for(unsigned size=0;size<4;++size)for(unsigned matrix=0;matrix<6;matrix+=size==3?3:1){
        unsigned mode=0,value=0;if(!bits.get(1,mode))return false;
        if(!mode){if(!bits.ue(matrix,value))return false;}
        else{
            if(size>1&&!bits.se(-7,247))return false;
            const unsigned count=std::min(64u,1u<<(4+2*size));
            for(unsigned i=0;i<count;++i)if(!bits.se(-128,127))return false;
        }
    }
    return true;
}
inline bool hevc_short_term_sets(Bits& bits,unsigned count) noexcept {
    std::array<unsigned,64> deltas{};
    for(unsigned i=0;i<count;++i){
        unsigned predicted=0,value=0;
        if(i&&!bits.get(1,predicted))return false;
        if(predicted){
            // In an SPS, delta_idx_minus1 is absent; the reference is i - 1.
            if(!bits.skip(1)||!bits.ue(65535,value))return false;
            for(unsigned j=0;j<=deltas[i-1];++j){
                unsigned used=0,use_delta=1;
                if(!bits.get(1,used)||(!used&&!bits.get(1,use_delta)))return false;
                if(used||use_delta)++deltas[i];
            }
            if(deltas[i]>64)return false;
        }else{
            unsigned negative=0,positive=0;
            if(!bits.ue(64,negative)||!bits.ue(64,positive)||negative+positive>64)return false;
            deltas[i]=negative+positive;
            for(unsigned j=0;j<deltas[i];++j)if(!bits.ue(65535,value)||!bits.skip(1))return false;
        }
    }
    return true;
}
} // namespace video_color_detail

// These bounded readers cover progressive 8-bit 4:2:0 SPS syntax used by the
// hardware decoder. Unsupported or truncated syntax returns false and leaves
// both outputs unchanged. They extract metadata, not a full SPS validation.
inline bool h264_sps_color(const std::uint8_t* nal,std::size_t size,
                           unsigned& sps_id,VideoColorInfo& output) noexcept {
    using namespace video_color_detail;
    if(!nal||size<2||size>max_sps_bytes||(nal[0]&0x80)||(nal[0]&31)!=7||!(nal[0]&0x60))return false;
    std::array<std::uint8_t,max_sps_bytes> rbsp{};std::size_t count=0;
    if(!unescape(nal+1,size-1,rbsp,count))return false;
    Bits bits{rbsp.data(),count};unsigned profile=0,value=0,id=0;
    if(!bits.get(8,profile)||!bits.get(8,value)||(value&3)||!bits.get(8,value)||!value||!bits.ue(31,id))return false;
    if(profile==100){
        unsigned chroma=0,luma=0,chroma_depth=0,scaling=0;
        if(!bits.ue(3,chroma)||chroma!=1||!bits.ue(6,luma)||luma||!bits.ue(6,chroma_depth)||chroma_depth
           ||!bits.skip(1)||!bits.get(1,scaling))return false;
        if(scaling)for(unsigned i=0;i<8;++i)if(!bits.get(1,value)||(value&&!avc_scaling(bits,i<6?16:64)))return false;
    }else if(profile!=66&&profile!=77&&profile!=88)return false;
    unsigned poc=0;
    if(!bits.ue(12,value)||!bits.ue(2,poc))return false;
    if(poc==0){if(!bits.ue(12,value))return false;}
    if(poc==1){unsigned cycle=0;
        if(!bits.skip(1)||!bits.se(-32768,32767)||!bits.se(-32768,32767)||!bits.ue(255,cycle))return false;
        for(unsigned i=0;i<cycle;++i)if(!bits.se(-32768,32767))return false;
    }
    unsigned frame_only=0,crop=0,vui=0;
    if(!bits.ue(16,value)||!bits.skip(1)||!bits.ue(511,value)||!bits.ue(511,value)
       ||!bits.get(1,frame_only)||!frame_only||!bits.skip(1)||!bits.get(1,crop))return false;
    if(crop)for(unsigned i=0;i<4;++i)if(!bits.ue(4096,value))return false;
    VideoColorInfo color{2,1};
    if(!bits.get(1,vui)||(vui&&!vui_color(bits,color)))return false;
    sps_id=id;output=color;return true;
}
inline bool hevc_sps_color(const std::uint8_t* nal,std::size_t size,
                           unsigned& sps_id,VideoColorInfo& output) noexcept {
    using namespace video_color_detail;
    if(!nal||size<3||size>max_sps_bytes||(nal[0]&0x80)||((nal[0]>>1)&63)!=33||!(nal[1]&7))return false;
    std::array<std::uint8_t,max_sps_bytes> rbsp{};std::size_t count=0;
    if(!unescape(nal+2,size-2,rbsp,count))return false;
    Bits bits{rbsp.data(),count};unsigned layers=0,id=0,value=0;
    if(!bits.skip(4)||!bits.get(3,layers)||layers>6||!bits.skip(1)||!hevc_profile(bits,layers)
       ||!bits.ue(15,id)||!bits.ue(3,value)||value!=1)return false;
    if(!bits.ue(8192,value)||!value||!bits.ue(8192,value)||!value||!bits.get(1,value))return false;
    if(value)for(unsigned i=0;i<4;++i)if(!bits.ue(8192,value))return false;
    unsigned luma=0,chroma=0,log_poc=0,ordering=0;
    if(!bits.ue(8,luma)||luma||!bits.ue(8,chroma)||chroma||!bits.ue(12,log_poc)||!bits.get(1,ordering))return false;
    for(unsigned i=ordering?0:layers;i<=layers;++i)
        if(!bits.ue(16,value)||!bits.ue(16,value)||!bits.ue(65535,value))return false;
    for(unsigned i=0;i<6;++i)if(!bits.ue(i<4?3u:8u,value))return false;
    unsigned scaling=0,pcm=0;
    if(!bits.get(1,scaling))return false;
    if(scaling){if(!bits.get(1,value)||(value&&!hevc_scaling(bits)))return false;}
    if(!bits.skip(2)||!bits.get(1,pcm))return false;
    if(pcm&&(!bits.skip(8)||!bits.ue(3,value)||!bits.ue(3,value)||!bits.skip(1)))return false;
    unsigned short_sets=0,long_present=0;
    if(!bits.ue(64,short_sets)||!hevc_short_term_sets(bits,short_sets)||!bits.get(1,long_present))return false;
    if(long_present){unsigned long_count=0;if(!bits.ue(32,long_count))return false;
        for(unsigned i=0;i<long_count;++i)if(!bits.skip(log_poc+5))return false;}
    unsigned vui=0;VideoColorInfo color{2,1};
    if(!bits.skip(2)||!bits.get(1,vui)||(vui&&!vui_color(bits,color)))return false;
    sps_id=id;output=color;return true;
}
} // namespace surfxr
