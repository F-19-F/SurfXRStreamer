#include "wire.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace surfxr::transport {
bool is_v21(const std::string& version){require(version=="20.14.1"||version=="21.0.0-dev14","surfxr_unsupported_protocol");return version=="21.0.0-dev14";}
namespace {
std::uint64_t rotate(std::uint64_t x,unsigned n){return (x<<n)|(x>>(64-n));}
void round(std::array<std::uint64_t,4>& v){
    v[0]+=v[1];v[1]=rotate(v[1],13);v[1]^=v[0];v[0]=rotate(v[0],32);
    v[2]+=v[3];v[3]=rotate(v[3],16);v[3]^=v[2];
    v[0]+=v[3];v[3]=rotate(v[3],21);v[3]^=v[0];
    v[2]+=v[1];v[1]=rotate(v[1],17);v[1]^=v[2];v[2]=rotate(v[2],32);
}
}
// Rust std DefaultHasher (SipHash-1-3, zero keys), str::hash appends 0xff.
std::uint64_t path_id(const std::string& s){
    std::array<std::uint64_t,4> v{0x736f6d6570736575ULL,0x646f72616e646f6dULL,0x6c7967656e657261ULL,0x7465646279746573ULL};
    Bytes input(s.begin(),s.end());input.push_back(255);
    std::size_t pos=0;
    const auto compress=[&](std::uint64_t m){v[3]^=m;round(v);v[0]^=m;};
    while(input.size()-pos>=8){std::uint64_t m=0;for(unsigned i=0;i<8;++i)m|=std::uint64_t(input[pos+i])<<(8*i);compress(m);pos+=8;}
    std::uint64_t tail=std::uint64_t(input.size()&255)<<56;
    for(unsigned i=0;pos+i<input.size();++i)tail|=std::uint64_t(input[pos+i])<<(8*i);
    compress(tail);v[2]^=255;round(v);round(v);round(v);return v[0]^v[1]^v[2]^v[3];
}
Bytes control(std::uint32_t type,bool v21){Writer w(v21);w.integer(type,4);return std::move(w.bytes);}
ServerControl server_control(const Bytes& bytes,bool v21){
    Reader r(bytes,v21);ServerControl result;result.type=static_cast<unsigned>(r.integer(4));
    switch(result.type){
    case 0:case 2:case 3:break;
    case 1:result.codec=static_cast<unsigned>(r.integer(4));require(result.codec<=1,"surfxr_unsupported_codec");result.config=r.blob();require(!result.config.empty(),"surfxr_empty_decoder_config");if(v21)r.string();break;
    case 4:if(v21)r.skip(r.remaining());else r.duration();break; // v21 RealTimeConfig is optional postprocessing, bounded by control framer.
    case 5:result.reserved=r.string(4096);break;
    case 6:result.config=r.blob();break; // negotiated extension reply; unknown tags remain ignored
    default:fail("surfxr_unknown_server_control");
    }
    r.end();return result;
}
VideoHeader video_header(Reader& r,bool v21,bool video_haptics,bool gaze,bool image_timestamps){
    VideoHeader result;result.timestamp=r.duration();result.has_views=v21;
    if(v21){
        for(unsigned i=0;i<2;++i){auto& p=result.poses[i];p.orientation={r.real(),r.real(),r.real(),r.real()};p.position={r.real(),r.real(),r.real()};
            TrackingPose check;check.pose=p;check.flags=OrientationValid|PositionValid;require(tracking_pose_valid(check),"surfxr_invalid_video_pose");
            const float left=r.real(),right=r.real(),up=r.real(),down=r.real();
            require(left<0&&right>0&&up>0&&down<0&&left>-1.56f&&right<1.56f&&up<1.56f&&down>-1.56f,"surfxr_invalid_video_fov");
            result.fov[i]={std::tan(up),-std::tan(down),-std::tan(left),std::tan(right)};
        }
        require(!r.boolean(),"surfxr_foveation_not_supported");
    }
    result.idr=r.boolean();
    if(video_haptics){require(v21,"surfxr_invalid_video_haptics");for(auto& amplitude:result.haptics){amplitude=r.real();require(amplitude>=0&&amplitude<=1,"surfxr_invalid_video_haptics");}}
    if(gaze){require(v21,"surfxr_invalid_gaze_header");for(auto& eye:result.crops)for(auto& coordinate:eye){
        // Raw little-endian u32, independent of bincode's variable integers.
        coordinate=0;for(unsigned i=0;i<4;++i)coordinate|=unsigned(r.integer(1))<<(8*i);
    }}
    result.image_timestamp=result.timestamp;
    if(image_timestamps){require(v21,"surfxr_invalid_image_timestamp");result.image_timestamp=0;
        for(unsigned i=0;i<8;++i)result.image_timestamp|=r.integer(1)<<(8*i);
        require(result.image_timestamp>0,"surfxr_invalid_image_timestamp");
    }
    return result;
}
Haptic haptic(const Bytes& b,bool v21){
    Reader r(b,v21);const auto id=r.integer(8);Haptic h;
    if(id==path_id("/user/hand/left"))h.hand=0;
    else if(id==path_id("/user/hand/right"))h.hand=1;
    else fail("surfxr_unknown_haptic_device");
    h.duration_ns=r.duration();h.frequency=r.real();h.amplitude=r.real();r.end();
    require(h.duration_ns<=5'000'000'000&&h.frequency>=0&&h.frequency<=1000&&h.amplitude>=0&&h.amplitude<=1,"surfxr_invalid_haptic");return h;
}
}
