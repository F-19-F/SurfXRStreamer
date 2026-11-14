#pragma once
#include "socket.hpp"
#include "surfxr/surfxr_desktop_crypto.hpp"
#include "surfxr/session.hpp"
#include <thread>

namespace surfxr::transport {
class DesktopAudio {
public:
    DesktopAudio(const SessionOptions& o,const StreamConfig& config,const desktop::Key& key,const SessionCallbacks& callbacks)
        :socket_(Socket::bind(o.local_address,config.priority_port,false)),cipher_(key),callbacks_(callbacks){
        socket_.connect_udp(o.host,config.priority_port);thread_=std::thread([this,config]{loop(config);});
    }
    ~DesktopAudio(){stop_.store(true);if(thread_.joinable())thread_.join();}
    void check(){if(failed_.load())fail("surfxr_desktop_audio_receive_failed");}
private:
    Socket socket_;desktop::Cipher cipher_;SessionCallbacks callbacks_;std::thread thread_;
    std::atomic_bool stop_{false},failed_{false};
    void loop(const StreamConfig& config){
        try{
            Reassembler packets(config.packet_size,true,192000,1u<<2,8'000'000);
            while(!stop_.load()){
                std::array<std::uint8_t,2048> b{};const auto n=socket_.read(b.data(),b.size(),false);std::optional<Packet> packet;
                if(n){Bytes plain;try{plain=cipher_.open(b.data(),n,false);}catch(const std::runtime_error&){continue;}packet=packets.accept(plain,now_ns());}
                else packet=packets.pop_ready(now_ns());
                if(packet){require(packet->bytes.size()%4==0,"surfxr_invalid_audio_packet");if(callbacks_.audio_pcm)callbacks_.audio_pcm(packet->bytes.data(),packet->bytes.size());}
                if(!n&&!packet)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }catch(...){failed_.store(true);}
    }
};
}
