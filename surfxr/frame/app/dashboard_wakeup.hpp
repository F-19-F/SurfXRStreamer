#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace surfxr::viewer {
// Wake the existing overlay owner; never stop the viewer or start a second UI.
inline bool show_dashboard(){
    const auto* env=std::getenv("XDG_RUNTIME_DIR");
    std::string root=env&&*env?env:"/run/user/"+std::to_string(::getuid());
    struct stat st{};
    const auto private_dir=[&](const std::string& path,unsigned denied){
        return !path.empty()&&path[0]=='/'&&!::lstat(path.c_str(),&st)&&
            S_ISDIR(st.st_mode)&&st.st_uid==::getuid()&&!(st.st_mode&denied);
    };
    if(!private_dir(root,0022)||!private_dir(root+"/surfxr-client",0077))return false;
    const auto path=root+"/surfxr-client/overlay.sock";
    if(::lstat(path.c_str(),&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=::getuid()||(st.st_mode&0077))return false;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(path.size()>=sizeof(address.sun_path))return false;
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    const int fd=::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return false;
    const bool sent=::sendto(fd,"show",4,MSG_NOSIGNAL,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==4;
    ::close(fd);return sent;
}
}
