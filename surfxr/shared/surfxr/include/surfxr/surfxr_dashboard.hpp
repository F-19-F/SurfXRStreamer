#pragma once
#include "surfxr/ports.hpp"
#include "surfxr/surfxr_desktop_crypto.hpp"
#include <algorithm>

namespace surfxr::transport::desktop {
constexpr unsigned dashboard_port=ports::dashboard;
constexpr std::array<std::uint8_t,8> dashboard_magic{'A','L','V','R','D','B','0','1'};
constexpr std::array<std::uint8_t,8> ticket_magic{'A','L','V','R','D','T','0','1'};
struct Ticket {Key key{},id{};unsigned port=ports::desktop_control,fps=60,bitrate=40;bool input=true,audio=true,unified=false,media_modes=false;};
inline Ticket dashboard_ticket(const Bytes& bytes){
    require(bytes.size()==70&&bytes[0]==0,"surfxr_dashboard_desktop_unavailable");
    Ticket result;std::copy_n(bytes.data()+1,32,result.key.begin());std::copy_n(bytes.data()+33,32,result.id.begin());
    result.port=bytes[65]|unsigned(bytes[66])<<8;result.input=bytes[67]&1;result.audio=bytes[67]&2;
    result.unified=bytes[67]&4;
    result.fps=bytes[68];result.bitrate=bytes[69];
    require(result.port>=1024&&result.port<=65533&&result.fps>=30&&result.fps<=144&&result.bitrate>=1&&result.bitrate<=200&&(bytes[67]&~7)==0,"surfxr_dashboard_invalid_ticket");
    return result;
}
}
