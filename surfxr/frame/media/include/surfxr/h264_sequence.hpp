#pragma once
#include "surfxr/video_color.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace surfxr {
struct H264SequenceInspection {
    bool valid=false,parameter_sets_changed=false,random_access=false,has_parameter_sets=false,has_vcl=false;
    // An IDR is usable only when its referenced PPS and SPS are available.
    bool random_access_ready=false;
    bool has_active_color=false;
    VideoColorInfo active_color;
    std::uint32_t width=0,height=0;
};
// tracker supports the progressive 8-bit 4:2:0 profiles used by the hardware
// path; it does not attempt to validate slice data or opaque SPS VUI contents.
// Cache updates are transactional, and exact EBSP bytes detect VUI-only changes.
class H264SequenceTracker {
public:
    static constexpr std::size_t max_parameter_nal=4096,max_parameter_bytes=65536,max_packet_bytes=16*1024*1024;
    void reset() noexcept {sets_.clear();}
    H264SequenceInspection inspect_packet(const std::uint8_t* bytes,std::size_t size){
        if(!bytes||!size||size>max_packet_bytes)return {};
        auto current=start_code(bytes,size,0);if(current.position==size)return {};
        for(std::size_t i=0;i<current.position;++i)if(bytes[i])return {};
        H264SequenceInspection result;std::vector<std::pair<unsigned,Parameter>> pending;
        std::size_t pending_bytes=0;unsigned idr_pps=256,color_pps=256;bool non_idr=false,color_pps_valid=true;
        while(current.position<size){
            const auto begin=current.position+current.length;
            const auto next=start_code(bytes,size,begin);auto end=next.position;
            while(end>begin&&bytes[end-1]==0)--end;
            if(end-begin<2)return {};
            const auto* nal=bytes+begin;const auto length=end-begin;const unsigned type=nal[0]&31;
            // Reject reserved/extension/partitioned syntax rather than guessing
            // whether this selected hardware path can decode it as ordinary AVC.
            if((nal[0]&0x80)||type==0||type>12||(type>=2&&type<=4))return {};
            if(type==7||type==8){
                if(!(nal[0]&0x60)||result.has_vcl||length>max_parameter_nal||length+4>max_parameter_bytes-pending_bytes)return {};
                std::array<std::uint8_t,max_parameter_nal> rbsp{};std::size_t count=0;
                if(!unescape(nal+1,length-1,rbsp.data(),count,rbsp.size(),false))return {};
                Bits bits{rbsp.data(),count};Parameter parameter;unsigned id=0;
                if(type==7){if(!sps(bits,id,parameter))return {};
                    unsigned color_id=0;parameter.color_valid=h264_sps_color(nal,length,color_id,parameter.color)&&color_id==id;}
                else if(!pps(bits,id,parameter))return {};
                parameter.bytes.assign(nal,nal+length);pending.emplace_back((type==7?0u:32u)+id,std::move(parameter));
                pending_bytes+=length+4;result.has_parameter_sets=true;
            }else if(type==1||type==5){
                result.has_vcl=true;non_idr|=type==1;
                // Only the bounded slice prefix is needed to select this
                // picture's PPS/SPS, including ordinary predicted pictures.
                std::array<std::uint8_t,32> rbsp{};std::size_t count=0;
                const bool unescaped=unescape(nal+1,length-1,rbsp.data(),count,rbsp.size(),true);
                Bits bits{rbsp.data(),count};unsigned first_mb=0,slice_type=0,pps_id=0;
                const bool prefix_valid=unescaped&&bits.ue(1048575,first_mb)&&bits.ue(9,slice_type)&&bits.ue(255,pps_id);
                if(!prefix_valid||(color_pps!=256&&color_pps!=pps_id))color_pps_valid=false;
                if(prefix_valid)color_pps=pps_id;
                if(type==5){
                    if(!(nal[0]&0x60))return {};
                    // Only the slice header prefix is needed. Never copy/scan
                    // hundreds of KiB of high-bitrate slice data into the cache.
                    if(!prefix_valid
                            ||(slice_type%5!=2&&slice_type%5!=4)||(idr_pps!=256&&idr_pps!=pps_id))return {};
                    idr_pps=pps_id;result.random_access=true;
                }
            }
            current=next;
        }
        if(result.random_access&&non_idr)return {};
        auto candidate=sets_;
        for(auto& entry:pending){
            const auto old=candidate.find(entry.first);
            if(old!=candidate.end()&&old->second.bytes!=entry.second.bytes)result.parameter_sets_changed=true;
            // A new SPS/PPS ID after initialization also needs a clean DPB.
            if(old==candidate.end()&&!sets_.empty()){
                const bool same_group=entry.first<32?sets_.begin()->first<32:sets_.rbegin()->first>=32;
                result.parameter_sets_changed|=same_group;
            }
            candidate[entry.first]=std::move(entry.second);
        }
        std::size_t total=0;
        for(const auto& entry:candidate){if(entry.second.bytes.size()+4>max_parameter_bytes-total)return {};total+=entry.second.bytes.size()+4;}
        if(result.random_access){
            const auto p=candidate.find(32+idr_pps);
            if(p!=candidate.end()){
                const auto s=candidate.find(p->second.sps_id);
                if(s!=candidate.end()){
                    result.random_access_ready=true;result.width=s->second.width;result.height=s->second.height;
                }
            }
        }
        if(result.has_vcl&&color_pps_valid&&color_pps!=256){
            const auto p=candidate.find(32+color_pps);
            if(p!=candidate.end()){
                const auto s=candidate.find(p->second.sps_id);
                if(s!=candidate.end()&&s->second.color_valid){result.has_active_color=true;result.active_color=s->second.color;}
            }
        }
        sets_=std::move(candidate);result.valid=true;return result;
    }
    std::vector<std::uint8_t> parameter_sets_annexb() const {
        std::vector<std::uint8_t> bytes;
        for(const auto& entry:sets_){bytes.insert(bytes.end(),{0,0,0,1});bytes.insert(bytes.end(),entry.second.bytes.begin(),entry.second.bytes.end());}
        return bytes; // SPS before PPS, then ascending bounded ID.
    }
private:
    struct Parameter {std::vector<std::uint8_t> bytes;unsigned sps_id=0,width=0,height=0;bool color_valid=false;VideoColorInfo color;};
    struct StartCode {std::size_t position,length;};
    std::map<unsigned,Parameter> sets_;
    static StartCode start_code(const std::uint8_t* bytes,std::size_t size,std::size_t from) noexcept {
        for(std::size_t i=from;i+2<size;++i)if(bytes[i]==0&&bytes[i+1]==0){
            if(bytes[i+2]==1)return {i,3};
            if(i+3<size&&bytes[i+2]==0&&bytes[i+3]==1)return {i,4};
        }
        return {size,0};
    }
    static bool unescape(const std::uint8_t* bytes,std::size_t size,std::uint8_t* output,std::size_t& count,std::size_t capacity,bool prefix) noexcept {
        unsigned zeros=0;
        for(std::size_t i=0;i<size;++i){
            if(count==capacity)return prefix;
            const auto byte=bytes[i];
            if(zeros>=2&&byte==3){if(i+1==size||bytes[i+1]>3)return false;zeros=0;continue;}
            if(zeros>=2&&byte<3)return false;
            output[count++]=byte;zeros=byte==0?zeros+1:0;
        }
        return true;
    }
    struct Bits {
        const std::uint8_t* data;std::size_t size,position=0;
        bool get(unsigned n,unsigned& value) noexcept {
            if(n>32||n>size*8-position)return false;
            value=0;for(unsigned i=0;i<n;++i){value=(value<<1)|((data[position/8]>>(7-position%8))&1);++position;}return true;
        }
        bool ue(unsigned maximum,unsigned& value) noexcept {
            unsigned zeros=0,bit=0;do{if(!get(1,bit))return false;if(!bit&&++zeros>30)return false;}while(!bit);
            unsigned suffix=0;if(!get(zeros,suffix))return false;value=((1u<<zeros)-1)+suffix;return value<=maximum;
        }
        bool se(int minimum,int maximum) noexcept {
            unsigned code=0;if(!ue(65535,code))return false;
            const int value=(code&1)?static_cast<int>((code+1)/2):-static_cast<int>(code/2);
            return value>=minimum&&value<=maximum;
        }
        bool trailing() const noexcept {
            if(position>=size*8||size*8-position>8)return false;
            Bits copy=*this;unsigned value=0;if(!copy.get(1,value)||!value)return false;
            while(copy.position<copy.size*8)if(!copy.get(1,value)||value)return false;
            return true;
        }
        bool scaling(unsigned count) noexcept {
            int last=8,next=8;
            for(unsigned i=0;i<count;++i){
                if(next){unsigned code=0;if(!ue(256,code))return false;
                    const int delta=(code&1)?static_cast<int>((code+1)/2):-static_cast<int>(code/2);
                    if(delta<-128||delta>127)return false;
                    next=(last+delta+256)%256;}
                if(next)last=next;
            }
            return true;
        }
    };
    static bool sps(Bits& bits,unsigned& id,Parameter& parameter) noexcept {
        unsigned profile=0,constraints=0,level=0,value=0;
        if(!bits.get(8,profile)||!bits.get(8,constraints)||(constraints&3)||!bits.get(8,level)||!level||!bits.ue(31,id))return false;
        if(profile==100){
            unsigned chroma=0,luma_depth=0,chroma_depth=0,scaling=0;
            if(!bits.ue(3,chroma)||chroma!=1||!bits.ue(6,luma_depth)||luma_depth||!bits.ue(6,chroma_depth)||chroma_depth
                    ||!bits.get(1,value)||!bits.get(1,scaling))return false;
            if(scaling)for(unsigned i=0;i<8;++i){if(!bits.get(1,value)||(value&&!bits.scaling(i<6?16:64)))return false;}
        }else if(profile!=66&&profile!=77&&profile!=88)return false;
        unsigned poc=0;
        if(!bits.ue(12,value)||!bits.ue(2,poc))return false;
        if(poc==0){if(!bits.ue(12,value))return false;}
        if(poc==1){unsigned cycle=0;if(!bits.get(1,value)||!bits.se(-32768,32767)||!bits.se(-32768,32767)||!bits.ue(255,cycle))return false;
            for(unsigned i=0;i<cycle;++i)if(!bits.se(-32768,32767))return false;}
        unsigned width=0,height=0,frame_only=0,crop=0;
        if(!bits.ue(16,value)||!bits.get(1,value)||!bits.ue(511,width)||!bits.ue(511,height)
                ||!bits.get(1,frame_only)||!frame_only||!bits.get(1,value)||!bits.get(1,crop))return false;
        width=(width+1)*16;height=(height+1)*16;
        if(crop){unsigned left=0,right=0,top=0,bottom=0;
            if(!bits.ue(4096,left)||!bits.ue(4096,right)||!bits.ue(4096,top)||!bits.ue(4096,bottom)
                    ||2*(left+right)>=width||2*(top+bottom)>=height)return false;
            width-=2*(left+right);height-=2*(top+bottom);}
        // VUI is opaque to this tracker but remains in exact change detection.
        if(!bits.get(1,value)||(!value&&!bits.trailing())||width<128||height<128||std::uint64_t{width}*height>16u*1024u*1024u)return false;
        parameter.width=width;parameter.height=height;return true;
    }
    static bool pps(Bits& bits,unsigned& id,Parameter& parameter) noexcept {
        unsigned value=0,groups=0;
        // NVENC's ordinary AVC path uses one slice group. FMO and extensions
        // are not silently treated as the same progressive hardware format.
        if(!(bits.ue(255,id)&&bits.ue(31,parameter.sps_id)&&bits.get(1,value)&&bits.get(1,value)
            &&bits.ue(7,groups)&&groups==0&&bits.ue(31,value)&&bits.ue(31,value)&&bits.get(1,value)
            &&bits.get(2,value)&&value<=2&&bits.se(-26,25)&&bits.se(-26,25)&&bits.se(-12,12)
            &&bits.get(1,value)&&bits.get(1,value)&&bits.get(1,value)))return false;
        if(!bits.trailing()){
            unsigned transform=0,scaling=0;if(!bits.get(1,transform)||!bits.get(1,scaling))return false;
            if(scaling)for(unsigned i=0;i<6+2*transform;++i)if(!bits.get(1,value)||(value&&!bits.scaling(i<6?16:64)))return false;
            if(!bits.se(-12,12))return false;
        }
        return bits.trailing();
    }
};
} // namespace surfxr
