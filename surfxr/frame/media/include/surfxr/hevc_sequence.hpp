#pragma once
#include "surfxr/video_color.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace surfxr {
struct HevcSequenceInspection {
    bool valid=false,parameter_sets_changed=false,random_access=false,random_access_ready=false,has_parameter_sets=false,has_vcl=false;
    bool has_vps=false,has_sps=false,has_pps=false;
    bool has_active_color=false;
    VideoColorInfo active_color;
};
// Complete Annex B access units only; parameter sets may be in separate access
// units. This is not a byte-fragment assembler or a full SPS/VUI validator.
// Parameter identities use RBSP IDs, while changes compare exact canonical EBSP
// bytes (no hashes). Only start codes and Annex B trailing zeros are excluded.
class HevcSequenceTracker {
public:
    static constexpr std::size_t max_parameter_nal=4096,max_parameter_bytes=65536,max_packet_bytes=32*1024*1024;
    void reset() noexcept {sets_.clear();colors_.clear();pps_sps_.clear();have_sps_=false;}
    HevcSequenceInspection inspect_packet(const std::uint8_t* bytes,std::size_t size){
        if(!bytes||!size||size>max_packet_bytes)return {};
        const auto first=start_code(bytes,size,0);if(first.position==size)return {};
        for(std::size_t i=0;i<first.position;++i)if(bytes[i])return {};
        HevcSequenceInspection result;std::vector<Parameter> pending;std::size_t pending_bytes=0;
        unsigned picture_pps=4096;bool color_prefix_valid=true;
        auto current=first;
        while(current.position<size){
            const auto begin=current.position+current.length;
            const auto next=start_code(bytes,size,begin);auto end=next.position;
            while(end>begin&&bytes[end-1]==0)--end;
            if(end-begin<2)return {};
            const auto* nal=bytes+begin;const auto length=end-begin;
            if((nal[0]&0x80)||(nal[1]&7)==0)return {};
            const unsigned type=(nal[0]>>1)&63,layer=((nal[0]&1)<<5)|(nal[1]>>3);
            result.has_vcl|=type<=31;result.random_access|=type>=16&&type<=23;
            if(type<=31){
                unsigned pps=0;
                if(!slice_pps(nal,length,type,pps)||(picture_pps!=4096&&picture_pps!=layer*64+pps))color_prefix_valid=false;
                else picture_pps=layer*64+pps;
            }
            if(type>=32&&type<=34){
                if(length>max_parameter_nal||length+4>max_parameter_bytes-pending_bytes)return {};
                unsigned id=0,sps=0;if(!parameter_id(nal,length,type,id,sps))return {};
                const unsigned key=(type-32)*4096+layer*64+id;
                Parameter parameter{key,std::vector<std::uint8_t>(nal,nal+length),sps};
                if(type==33){unsigned color_id=0;parameter.color_valid=hevc_sps_color(nal,length,color_id,parameter.color)&&color_id==id;}
                pending.push_back(std::move(parameter));pending_bytes+=length+4;
                // A late parameter set belongs to subsequent pictures; never
                // assign its colour to a slice that preceded it in this AU.
                if(result.has_vcl)color_prefix_valid=false;
                result.has_parameter_sets=true;result.has_vps|=type==32;result.has_sps|=type==33;result.has_pps|=type==34;
            }
            current=next;
        }
        // A malformed tail never partially advances the sequence baseline.
        if(!pending.empty()){
            auto candidate=sets_;
            auto candidate_colors=colors_;auto candidate_pps=pps_sps_;
            for(auto& parameter:pending){
                const auto found=candidate.find(parameter.key);
                const auto group=parameter.key/64;
                const auto first_in_group=candidate.lower_bound(group*64);
                const bool known_group=first_in_group!=candidate.end()&&first_in_group->first/64==group;
                if(have_sps_&&((found!=candidate.end()&&found->second!=parameter.bytes)||(found==candidate.end()&&known_group)))result.parameter_sets_changed=true;
                const unsigned type=parameter.key/4096,key=parameter.key%4096;
                if(type==1){
                    if(parameter.color_valid)candidate_colors[key]=parameter.color;
                    else candidate_colors.erase(key);
                }else if(type==2)candidate_pps[key]=parameter.sps_id;
                candidate[parameter.key]=std::move(parameter.bytes);
            }
            std::size_t total=0;
            for(const auto& entry:candidate){if(entry.second.size()+4>max_parameter_bytes-total)return {};total+=entry.second.size()+4;}
            sets_=std::move(candidate);colors_=std::move(candidate_colors);pps_sps_=std::move(candidate_pps);have_sps_|=result.has_sps;
        }
        if(result.has_vcl&&color_prefix_valid&&picture_pps!=4096){
            const auto p=pps_sps_.find(picture_pps);
            if(p!=pps_sps_.end()){
                const auto layer=(picture_pps/64)*64;
                const auto vps=sets_.lower_bound(layer);
                result.random_access_ready=result.random_access&&vps!=sets_.end()&&vps->first/64==layer/64&&
                    sets_.count(4096+layer+p->second)!=0;
                const auto s=colors_.find((picture_pps/64)*64+p->second);
                if(s!=colors_.end()){result.has_active_color=true;result.active_color=s->second;}
            }
        }
        result.valid=true;return result;
    }
    std::vector<std::uint8_t> parameter_sets_annexb() const {
        std::vector<std::uint8_t> result;
        for(const auto& entry:sets_){result.insert(result.end(),{0,0,0,1});result.insert(result.end(),entry.second.begin(),entry.second.end());}
        return result; // <= max_parameter_bytes, VPS then SPS then PPS, by layer/ID.
    }
    bool has_sps() const noexcept {return have_sps_;}
private:
    struct StartCode {std::size_t position,length;};
    struct Parameter {unsigned key;std::vector<std::uint8_t> bytes;unsigned sps_id=0;bool color_valid=false;VideoColorInfo color{};};
    std::map<unsigned,std::vector<std::uint8_t>> sets_;
    std::map<unsigned,VideoColorInfo> colors_;
    std::map<unsigned,unsigned> pps_sps_;
    bool have_sps_=false;
    static StartCode start_code(const std::uint8_t* bytes,std::size_t size,std::size_t from) noexcept {
        if(from>=size||size-from<3)return {size,0};
        std::size_t at=from+2;
        while(at<size){
            auto end=size;
            if(size-at>=sizeof(std::uint64_t)){
                std::uint64_t word;std::memcpy(&word,bytes+at-2,sizeof(word));
                // Every candidate needs a zero two bytes before its final 01.
                // Skip eight when that window has none. The shifted window
                // covers that prerequisite even for the final two candidates.
                // Loads are bounded, unaligned-safe and endian-independent.
                // Candidate words still use exact bytes: borrow bits may give
                // false positives, and prefixes can cross a word boundary.
                constexpr std::uint64_t ones=0x0101010101010101ULL,high=0x8080808080808080ULL;
                if(((word-ones)&~word&high)==0){at+=sizeof(word);continue;}
                end=at+sizeof(word);
            }
            for(;at<end;++at)if(bytes[at]==1&&bytes[at-1]==0&&bytes[at-2]==0){
                const bool four=at-from>=3&&bytes[at-3]==0;
                return {at-(four?3u:2u),four?4u:3u};
            }
        }
        return {size,0};
    }
    struct Bits {
        const std::uint8_t* data;std::size_t size,position=0;
        bool skip(std::size_t n) noexcept {if(n>size*8-position)return false;position+=n;return true;}
        bool get(unsigned n,unsigned& value) noexcept {
            if(n>32||n>size*8-position)return false;
            value=0;for(unsigned i=0;i<n;++i){value=(value<<1)|((data[position/8]>>(7-position%8))&1);++position;}return true;
        }
        bool ue(unsigned maximum,unsigned& value) noexcept {
            unsigned zeros=0,bit=0;
            do{if(!get(1,bit))return false;if(!bit&&++zeros>16)return false;}while(!bit);
            unsigned suffix=0;if(!get(zeros,suffix))return false;
            value=((1u<<zeros)-1)+suffix;return value<=maximum;
        }
    };
    static bool slice_pps(const std::uint8_t* nal,std::size_t size,unsigned type,unsigned& pps) noexcept {
        std::array<std::uint8_t,16> prefix{};std::size_t length=0;unsigned zeros=0;
        for(std::size_t i=2;i<size&&length<prefix.size();++i){const auto byte=nal[i];
            if(zeros>=2&&byte==3){if(i+1==size||nal[i+1]>3)return false;zeros=0;continue;}
            if(zeros>=2&&byte<3)return false;
            prefix[length++]=byte;zeros=byte==0?zeros+1:0;
        }
        Bits bits{prefix.data(),length};
        // first_slice_segment_in_pic_flag, optional no_output_of_prior_pics,
        // then slice_pic_parameter_set_id, present also on dependent slices.
        return bits.skip(1)&&(!(type>=16&&type<=23)||bits.skip(1))&&bits.ue(63,pps);
    }
    static bool parameter_id(const std::uint8_t* nal,std::size_t size,unsigned type,unsigned& id,unsigned& sps) noexcept {
        std::array<std::uint8_t,max_parameter_nal> rbsp{};std::size_t length=0;unsigned zeros=0;
        for(std::size_t i=2;i<size;++i){const auto byte=nal[i];
            if(zeros>=2&&byte==3){if(i+1==size||nal[i+1]>3)return false;zeros=0;continue;}
            if(zeros>=2&&byte<3)return false;
            rbsp[length++]=byte;zeros=byte==0?zeros+1:0;
        }
        Bits bits{rbsp.data(),length};
        if(type==32)return bits.get(4,id);
        if(type==34)return bits.ue(63,id)&&bits.ue(15,sps);
        // SPS: VPS ID, sublayer count, temporal nesting, then profile_tier_level.
        // Its general profile is 88 bits plus 8-bit level; each optional sublayer
        // profile and level has the same fixed widths. VUI remains opaque.
        unsigned layers=0;if(!bits.skip(4)||!bits.get(3,layers)||layers>6||!bits.skip(1)||!bits.skip(96))return false;
        std::array<unsigned,7> profile{},level{};
        for(unsigned i=0;i<layers;++i)if(!bits.get(1,profile[i])||!bits.get(1,level[i]))return false;
        if(layers&&!bits.skip((8-layers)*2))return false;
        for(unsigned i=0;i<layers;++i)if((profile[i]&&!bits.skip(88))||(level[i]&&!bits.skip(8)))return false;
        return bits.ue(15,id);
    }
};
} // namespace surfxr
