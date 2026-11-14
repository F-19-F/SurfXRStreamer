#include "surfxr/system_input.hpp"
#include <cstdlib>
#include <ctime>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace surfxr::frame_input {
std::uint64_t monotonic_ns() noexcept {
    timespec time{};if(clock_gettime(CLOCK_MONOTONIC,&time))return 0;
    return std::uint64_t{static_cast<std::uint64_t>(time.tv_sec)}*1'000'000'000+time.tv_nsec;
}
std::string socket_path(){
    const auto* root=std::getenv("XDG_RUNTIME_DIR");
    return (root&&*root?std::string(root):"/run/user/"+std::to_string(getuid()))+"/surfxr-client/system-input.sock";
}
int private_directory(){
    const auto path=socket_path();const auto directory=path.substr(0,path.find_last_of('/'));
    if(directory.empty()||directory.front()!='/')return -1;
    ::mkdir(directory.c_str(),0700);
    struct stat st{};return lstat(directory.c_str(),&st)==0&&S_ISDIR(st.st_mode)&&st.st_uid==getuid()&&
        (st.st_mode&077)==0?0:-1;
}
bool same_user(int socket) noexcept {
    ucred credentials{};socklen_t size=sizeof(credentials);
    return getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&credentials,&size)==0&&size==sizeof(credentials)&&credentials.uid==getuid();
}
}
