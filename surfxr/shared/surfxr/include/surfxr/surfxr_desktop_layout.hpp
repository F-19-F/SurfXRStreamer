#pragma once
#include "surfxr/surfxr_desktop.hpp"

namespace surfxr::transport::desktop {
// Explicitly negotiated extension. Legacy mono control/video bytes stay intact.
inline constexpr unsigned layout_type=14,input_type=12,monitor_type=13;
inline constexpr std::array<unsigned,4> video_streams{3,0,1,4};
struct Stream {Display display;unsigned codec=2;Bytes configuration;};
struct Layout {unsigned generation=1,fps=60,capacity=1;std::vector<Stream> streams;};
inline void validate(const Layout& l){
    require(l.generation&&l.fps>=30&&l.fps<=144&&l.capacity>=1&&l.capacity<=4&&
        !l.streams.empty()&&l.streams.size()<=l.capacity,"surfxr_desktop_invalid_layout");
    for(const auto& s:l.streams){const auto& d=s.display;
        require(d.x>=-65536&&d.x<=65536&&d.y>=-65536&&d.y<=65536&&d.width>=32&&d.width<=16384&&d.height>=32&&d.height<=16384&&
            d.encoded_width>=32&&d.encoded_width<=4096&&d.encoded_height>=32&&d.encoded_height<=4096&&
            std::uint64_t{d.encoded_width}*d.encoded_height<=16*1024*1024&&s.codec>=1&&s.codec<=2&&
            !s.configuration.empty()&&s.configuration.size()<=65536,"surfxr_desktop_invalid_layout_stream");
    }
}
inline Bytes layout(const Layout& l){
    validate(l);Writer w(true);w.integer(layout_type,4);w.integer(1,4);w.integer(l.generation,4);
    w.integer(l.fps,4);w.integer(l.capacity,4);w.integer(l.streams.size(),4);
    for(const auto& s:l.streams){const auto& d=s.display;w.integer(s.codec,4);
        w.integer(d.encoded_width,4);w.integer(d.encoded_height,4);
        for(int n:{d.x,d.y,d.width,d.height})w.integer(static_cast<std::uint32_t>(n),4);
        w.blob(s.configuration);
    }return w.bytes;
}
inline Layout layout(const Bytes& bytes){
    Reader r(bytes,true);require(r.integer(4)==layout_type&&r.integer(4)==1,"surfxr_desktop_layout_version");Layout l;
    l.generation=static_cast<unsigned>(r.integer(4));l.fps=static_cast<unsigned>(r.integer(4));l.capacity=static_cast<unsigned>(r.integer(4));
    const auto count=r.integer(4);require(count>=1&&count<=4,"surfxr_desktop_invalid_layout");
    for(unsigned i=0;i<count;++i){Stream s;s.codec=static_cast<unsigned>(r.integer(4));auto& d=s.display;
        d.encoded_width=static_cast<unsigned>(r.integer(4));d.encoded_height=static_cast<unsigned>(r.integer(4));
        d.x=static_cast<std::int32_t>(r.integer(4));d.y=static_cast<std::int32_t>(r.integer(4));
        d.width=static_cast<std::int32_t>(r.integer(4));d.height=static_cast<std::int32_t>(r.integer(4));
        s.configuration=r.blob(65536);l.streams.push_back(std::move(s));
    }r.end();validate(l);return l;
}
inline Bytes input(const Event& e,unsigned generation){
    auto bytes=input(e);Reader r(bytes,true);r.integer(4);Writer w(true);
    w.integer(input_type,4);w.integer(generation,4);w.blob(r.blob(64));return w.bytes;
}
inline Bytes monitor_request(bool add){Writer w(true);w.integer(monitor_type,4);w.boolean(add);return w.bytes;}
inline Bytes video(unsigned generation,std::uint64_t stamp,bool idr,const std::uint8_t* data,std::size_t size){
    Writer w(true);w.integer(generation,4);auto picture=video(stamp,idr,data,size);w.bytes.insert(w.bytes.end(),picture.begin(),picture.end());return w.bytes;
}
}
