#pragma once
#include "surfxr/surfxr_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>

namespace surfxr {
struct KeyboardImage {
    unsigned width=0,height=0;
    std::uint64_t token=0;
    std::vector<std::uint8_t> rgba;
    std::chrono::steady_clock::time_point received=std::chrono::steady_clock::now();
};
// Latest immutable image only; receiver never waits on the XR owner.
struct KeyboardOverlay {
    std::atomic_bool active{false},supported{false};
    std::atomic_bool clickable{false};
    std::atomic_uint64_t received_images{0};
    std::shared_ptr<const KeyboardImage> image;
    void publish(std::shared_ptr<const KeyboardImage> value){if(value)++received_images;std::atomic_store(&image,std::move(value));}
    std::shared_ptr<const KeyboardImage> snapshot()const{return std::atomic_load(&image);}
};
namespace keyboard_image {
inline constexpr unsigned max_width=1024,max_height=256,max_bytes=65536;
inline constexpr std::array<std::uint8_t,8> magic{'F','R','I','M','E','0','0','1'};
inline transport::Bytes identify(transport::Bytes image,std::uint64_t token){
    if(image.empty()||image.size()+8>max_bytes)return {};
    transport::require(image.size()>=4&&token,"keyboard_image_identity");
    image[1]|=0x80;transport::Writer w;w.integer(token,8);image.insert(image.begin()+4,w.bytes.begin(),w.bytes.end());return image;
}
// RLE RGB is lossless for small candidate text. No font dependency on Frame.
inline transport::Bytes encode(unsigned width,unsigned height,const std::uint8_t* bgra){
    using namespace transport;require(width&&height&&width<=max_width&&height<=max_height,"keyboard_image_dimensions");
    Writer w;w.integer(width,2);w.integer(height,2);
    for(unsigned at=0;at<width*height;){unsigned count=1;
        while(count<65535&&at+count<width*height&&std::equal(bgra+at*4,bgra+at*4+3,bgra+(at+count)*4))++count;
        w.integer(count,2);w.integer(bgra[at*4+2],1);w.integer(bgra[at*4+1],1);w.integer(bgra[at*4],1);at+=count;
        if(w.bytes.size()>max_bytes)return {}; // fail closed, never send unbounded captures
    }return w.bytes;
}
inline std::shared_ptr<const KeyboardImage> decode(const std::uint8_t* bytes,std::size_t size){
    using namespace transport;require(size<=max_bytes,"keyboard_image_bound");
    if(!size)return {};
    Reader r(bytes,size);auto image=std::make_shared<KeyboardImage>();
    image->width=static_cast<unsigned>(r.integer(2));image->height=static_cast<unsigned>(r.integer(2));
    if(image->width&0x8000){image->width&=0x7fff;image->token=r.integer(8);require(image->token!=0,"keyboard_image_token");}
    require(image->width&&image->height&&image->width<=max_width&&image->height<=max_height,"keyboard_image_dimensions");
    const auto pixels=image->width*image->height;image->rgba.reserve(pixels*4);
    unsigned at=0;while(r.remaining()){
        const auto n=static_cast<unsigned>(r.integer(2));require(n&&n<=pixels-at,"keyboard_image_run");
        const auto red=r.integer(1),green=r.integer(1),blue=r.integer(1);
        for(unsigned i=0;i<n;++i)image->rgba.insert(image->rgba.end(),{std::uint8_t(red),std::uint8_t(green),std::uint8_t(blue),255});
        at+=n;
    }require(at==pixels,"keyboard_image_incomplete");return image;
}
// Remove the negotiated auxiliary suffix before passing the AU to hardware.
// The SURFXR header, source timestamp, FEC, encoder and stream ID remain intact.
inline bool extract(transport::Bytes& packet,std::size_t video_offset,KeyboardOverlay& overlay){
    using namespace transport;
    if(packet.size()<video_offset+12||!std::equal(magic.begin(),magic.end(),packet.end()-8))return false;
    Reader r(packet.data()+packet.size()-12,4);const auto size=r.integer(4);
    require(size<=max_bytes&&size<packet.size()-video_offset-12,"keyboard_image_trailer");
    const auto start=packet.size()-12-static_cast<std::size_t>(size);
    overlay.publish(decode(packet.data()+start,static_cast<std::size_t>(size)));packet.resize(start);return true;
}
} // namespace keyboard_image
} // namespace surfxr
