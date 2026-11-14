#pragma once
#include "discovery_scan.hpp"

namespace surfxr::discovery_detail {
// Companion SURFXR v21 PC discovery. A nonce associates replies with this scan;
// discovery is not authentication and never changes the PC's trust list.
class SurfxrReplies {
public:
    explicit SurfxrReplies(unsigned port):port_(port){
        request.assign({'A','L','V','R','F','D','0','1'});request.resize(16);
        require(RAND_bytes(request.data()+8,8)==1,"scan_random_failed");
    }
    bool accept(View packet,const sockaddr_in& sender){
        if(sender.sin_family!=AF_INET||ntohs(sender.sin_port)!=port_||packet.size<27||packet.size>90||
           std::memcmp(packet.data,"ALVRPC01",8)||std::memcmp(packet.data+8,request.data()+8,8)||
           std::memcmp(packet.data+16,"21-dev14",8)||
           (unsigned(packet.data[24])|(unsigned(packet.data[25])<<8))!=port_||
           packet.data[26]>63||packet.size!=27u+packet.data[26]||reports.size()>=8)return false;
        const auto ip=sender.sin_addr.s_addr;
        if(std::find(seen_.begin(),seen_.end(),ip)!=seen_.end())return false;
        std::array<char,INET_ADDRSTRLEN> text{};
        if(!inet_ntop(AF_INET,&sender.sin_addr,text.data(),text.size()))return false;
        // Names are optional presentation hints; never inject peer strings into JSON.
        reports.push_back("{\"status\":\"discovered\",\"backend\":\"surfxr\",\"source\":\""+std::string(text.data())+
            "\",\"connection_started\":false}");seen_.push_back(ip);return true;
    }
    Bytes request;
    std::vector<std::string> reports;
private:
    unsigned port_;
    std::vector<std::uint32_t> seen_;
};
std::string scan_surfxr(const char* local,unsigned prefix,unsigned port,const std::string& host,const char*& stage);
}
