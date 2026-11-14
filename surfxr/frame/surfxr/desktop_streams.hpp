#pragma once
#include "surfxr/surfxr_desktop_layout.hpp"
#include "surfxr/session.hpp"
#include "surfxr/surfxr_stream.hpp"
#include "wire.hpp"
#include <algorithm>

namespace surfxr::transport {
// The receiver owns references/configuration independently for each panel.
// A layout generation fences late UDP pictures after remove/re-add or UAC.
class DesktopStreams {
public:
    explicit DesktopStreams(const SessionCallbacks& callbacks):callbacks_(callbacks){}
    unsigned generation()const{return layout_.generation;}
    void configure(const Bytes& bytes){
        auto next=desktop::layout(bytes);
        require(!configured_||next.generation>layout_.generation,"surfxr_desktop_layout_generation");
        layout_=std::move(next);configured_=true;corrupted_.fill(true);
        if(callbacks_.desktop_input_release)callbacks_.desktop_input_release();
        DesktopConfiguration configuration;configuration.display_frequency=layout_.fps;configuration.monitor_capacity=layout_.capacity;
        for(unsigned id=0;id<layout_.streams.size();++id){const auto& s=layout_.streams[id];const auto& d=s.display;
            configuration.streams.push_back({id,s.codec,d.encoded_width,d.encoded_height,d.x,d.y,d.width,d.height});
        }
        if(callbacks_.desktop_config)callbacks_.desktop_config(configuration);
        if(callbacks_.source_changed)callbacks_.source_changed(0);
    }
    bool receive(const Packet& packet,const std::function<void()>& idr){
        if(!configured_)return false;
        const auto found=std::find(desktop::video_streams.begin(),desktop::video_streams.end(),packet.stream);
        require(found!=desktop::video_streams.end(),"surfxr_desktop_video_stream");
        const auto id=static_cast<unsigned>(found-desktop::video_streams.begin());Reader r(packet.bytes,true);
        if(r.integer(4)!=layout_.generation||id>=layout_.streams.size())return false;
        const auto header=video_header(r,true);
        if(packet.loss){corrupted_[id]=true;idr();}if(header.idr)corrupted_[id]=false;
        if(corrupted_[id]){idr();return false;}
        require(r.remaining()>0,"surfxr_empty_video_packet");Bytes access;
        if(header.idr){access=layout_.streams[id].configuration;access.insert(access.end(),r.data(),r.data()+r.remaining());}
        if(callbacks_.video_packet)callbacks_.video_packet(id,header.idr?access.data():r.data(),header.idr?access.size():r.remaining(),header.timestamp);
        return true;
    }
private:
    const SessionCallbacks& callbacks_;desktop::Layout layout_;
    bool configured_=false;std::array<bool,4> corrupted_{true,true,true,true};
};
}
