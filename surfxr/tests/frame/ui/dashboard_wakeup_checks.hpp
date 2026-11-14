#pragma once
#include "../../../frame/app/dashboard_wakeup.hpp"
#include <stdexcept>
namespace surfxr {
inline void dashboard_wakeup_checks(){
    const auto check=[](bool v){if(!v)throw std::runtime_error("dashboard_wakeup_fixture");};
    const auto* env=std::getenv("XDG_RUNTIME_DIR");const bool had=env;const std::string previous=env?env:"";
    char pattern[]="/tmp/surfxr-dashboard-wakeup-XXXXXX";const auto* made=::mkdtemp(pattern);check(made);
    const std::string root=made,directory=root+"/surfxr-client",path=directory+"/overlay.sock";
    struct Cleanup {std::string root,directory,path,previous;bool had;int fd=-1;
        ~Cleanup(){if(fd>=0)::close(fd);::unlink(path.c_str());::rmdir(directory.c_str());::rmdir(root.c_str());if(had)::setenv("XDG_RUNTIME_DIR",previous.c_str(),1);else ::unsetenv("XDG_RUNTIME_DIR");}
    } cleanup{root,directory,path,previous,had};
    check(::setenv("XDG_RUNTIME_DIR",root.c_str(),1)==0&&::mkdir(directory.c_str(),0700)==0);
    check(!viewer::show_dashboard());
    cleanup.fd=::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);check(cleanup.fd>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;check(path.size()<sizeof(address.sun_path));std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    check(::bind(cleanup.fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0&&::chmod(path.c_str(),0600)==0);
    check(viewer::show_dashboard());char message[8]{};check(::recv(cleanup.fd,message,sizeof(message),0)==4&&std::string(message)=="show");
    check(::chmod(path.c_str(),0666)==0&&!viewer::show_dashboard());
    check(::chmod(path.c_str(),0600)==0&&::chmod(directory.c_str(),0755)==0&&!viewer::show_dashboard());
}
}
