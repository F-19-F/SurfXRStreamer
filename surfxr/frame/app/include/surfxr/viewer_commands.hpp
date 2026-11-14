#pragma once
#include "surfxr/audio_output.hpp"
#include "surfxr/frame_timing.hpp"
#include "surfxr/input_preferences.hpp"
#include "surfxr/remote_settings.hpp"
#include "surfxr/session.hpp"
#include "surfxr/test_head_motion.hpp"
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <cstdint>
#include <thread>
#include <optional>
#include <array>
#include <charconv>
#include <cstring>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

namespace surfxr {
// Private telemetry file contract, mirrored by MAX_VIEWER_STATUS_BYTES in the
// launcher. IPC requests/responses keep their separate, smaller bounds.
inline constexpr std::size_t viewer_status_max_bytes=16*1024;
// Covers daemon, desktop and manual invocations with one stable lock inode.
class ViewerInstanceLock {
public:
    static std::string runtime_directory(){
        const char* value=std::getenv("XDG_RUNTIME_DIR");
        std::string root=value&&value[0]?value:"/run/user/"+std::to_string(getuid());
        struct stat info{};
        if(root.empty()||root[0]!='/'||lstat(root.c_str(),&info)!=0||!S_ISDIR(info.st_mode)||info.st_uid!=getuid()||(info.st_mode&0022))
            throw std::runtime_error("viewer_runtime_directory_invalid");
        root+="/surfxr-client";
        if(mkdir(root.c_str(),0700)!=0&&errno!=EEXIST)throw std::runtime_error("viewer_runtime_directory_failed");
        return root;
    }
    explicit ViewerInstanceLock(const std::string& directory=runtime_directory()){
        struct stat info{};
        if(lstat(directory.c_str(),&info)!=0||!S_ISDIR(info.st_mode)||info.st_uid!=getuid()||(info.st_mode&0077))
            throw std::runtime_error("viewer_runtime_directory_not_private");
        fd_=open((directory+"/viewer.lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd_<0)throw std::runtime_error("viewer_lock_failed");
        if(fstat(fd_,&info)!=0||!S_ISREG(info.st_mode)||info.st_uid!=getuid()||(info.st_mode&0077)||flock(fd_,LOCK_EX|LOCK_NB)!=0){
            close(fd_);fd_=-1;throw std::runtime_error("viewer_already_running_or_lock_unavailable");
        }
    }
    ~ViewerInstanceLock(){if(fd_>=0)close(fd_);}
    ViewerInstanceLock(const ViewerInstanceLock&)=delete;
    ViewerInstanceLock& operator=(const ViewerInstanceLock&)=delete;
private:
    int fd_=-1;
};
// Created only after ViewerInstanceLock is held. Requests are absolute modes,
// coalesced without waiting on the render thread; no remote endpoint is opened.
class ViewerModeCommands {
public:
    explicit ViewerModeCommands(const std::string& directory=ViewerInstanceLock::runtime_directory()){
        struct stat info{};
        if(lstat(directory.c_str(),&info)!=0||!S_ISDIR(info.st_mode)||info.st_uid!=getuid()||(info.st_mode&0077))
            throw std::runtime_error("viewer_command_directory_not_private");
        path_=directory+"/viewer-command.sock";
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(path_.size()>=sizeof(address.sun_path))throw std::runtime_error("viewer_command_path_too_long");
        if(lstat(path_.c_str(),&info)==0){
            if(!S_ISSOCK(info.st_mode)||info.st_uid!=getuid())throw std::runtime_error("viewer_command_path_invalid");
            if(unlink(path_.c_str())!=0)throw std::runtime_error("viewer_command_cleanup_failed");
        }else if(errno!=ENOENT)throw std::runtime_error("viewer_command_path_unavailable");
        fd_=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd_<0)throw std::runtime_error("viewer_command_socket_failed");
        const int credentials=1;
        std::memcpy(address.sun_path,path_.c_str(),path_.size()+1);
        if(setsockopt(fd_,SOL_SOCKET,SO_PASSCRED,&credentials,sizeof(credentials))!=0||
           bind(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){
            close(fd_);fd_=-1;throw std::runtime_error("viewer_command_bind_failed");
        }
        if(chmod(path_.c_str(),0600)!=0){close(fd_);fd_=-1;unlink(path_.c_str());throw std::runtime_error("viewer_command_permissions_failed");}
    }
    ~ViewerModeCommands(){if(fd_>=0){close(fd_);unlink(path_.c_str());}}
    ViewerModeCommands(const ViewerModeCommands&)=delete;
    ViewerModeCommands& operator=(const ViewerModeCommands&)=delete;
    std::optional<bool> poll() noexcept {
        std::optional<bool> result;
        for(unsigned i=0;i<16;++i){
            char payload[32]{};
            alignas(cmsghdr) char credentials[CMSG_SPACE(sizeof(ucred))]{};
            iovec vector{payload,sizeof(payload)};msghdr message{};
            message.msg_iov=&vector;message.msg_iovlen=1;message.msg_control=credentials;message.msg_controllen=sizeof(credentials);
            const auto count=recvmsg(fd_,&message,MSG_DONTWAIT);
            if(count<0){if(errno==EINTR)continue;break;}
            if(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))continue;
            bool same_user=false;
            for(auto* item=CMSG_FIRSTHDR(&message);item;item=CMSG_NXTHDR(&message,item)){
                if(item->cmsg_level==SOL_SOCKET&&item->cmsg_type==SCM_CREDENTIALS&&item->cmsg_len>=CMSG_LEN(sizeof(ucred))){
                    ucred sender{};std::memcpy(&sender,CMSG_DATA(item),sizeof(sender));same_user=sender.uid==getuid();
                }
            }
            if(!same_user)continue;
            if(count==12&&std::memcmp(payload,"exit-steamvr",12)==0){exit_=true;continue;}
            if(count==3&&std::memcmp(payload,"vr\n",3)==0)result=true;
            else if(count==8&&std::memcmp(payload,"desktop\n",8)==0)result=false;
            else if(count==9&&std::memcmp(payload,"timing 1\n",9)==0)timing_=true;
            else if(count==9&&std::memcmp(payload,"timing 0\n",9)==0)timing_=false;
            else if(count==9&&std::memcmp(payload,"pacing 1\n",9)==0)pacing_=true;
            else if(count==9&&std::memcmp(payload,"pacing 0\n",9)==0)pacing_=false;
            else if(count==12&&std::memcmp(payload,"nv12-fuse ",10)==0&&(payload[10]=='0'||payload[10]=='1')&&payload[11]=='\n')nv12_fused_=payload[10]=='1';
            else if(count==11&&std::memcmp(payload,"monitors ",9)==0&&payload[9]>='1'&&payload[9]<='4'&&payload[10]=='\n')monitors_=static_cast<unsigned>(payload[9]-'0');
            else if(count>=8&&std::memcmp(payload,"set ",4)==0&&payload[count-1]=='\n'){
                // Allowlisted numeric preferences, never arbitrary JSON
                // or filesystem input. Coalesce each row in bounded storage.
                const char* end=payload+count-1;int id=-1,value=0;
                const auto key=std::from_chars(payload+4,end,id);
                if(key.ec!=std::errc{}||key.ptr==end||*key.ptr!=' '||id<0||(id>2&&id!=7&&id!=9))continue;
                const auto number=std::from_chars(key.ptr+1,end,value);
                if(number.ec!=std::errc{}||number.ptr!=end)continue;
                const bool legal=id==0?(value==24||value==30||value==60||value==72||value==80||value==90||value==96||value==100||value==120||value==144):
                    id==1?(value>=-1&&value<=7):(id==7||id==9)?(value==0||value==1):(value>=5&&value<=1000);
                if(legal)settings_[static_cast<unsigned>(id)]=value;
            }else if(count>=10&&std::memcmp(payload,"input ",6)==0&&payload[count-1]=='\n'){
                const char* end=payload+count-1;int id=-1,value=0;
                const auto key=std::from_chars(payload+6,end,id);
                if(key.ec!=std::errc{}||key.ptr==end||*key.ptr!=' '||payload[6]<'0'||payload[6]>'9'||id<0||static_cast<unsigned>(id)>=kInputPreferenceCount||
                   (key.ptr>payload+7&&payload[6]=='0'))continue;
                const auto number=std::from_chars(key.ptr+1,end,value);
                if(number.ec!=std::errc{}||number.ptr!=end||key.ptr[1]<'0'||key.ptr[1]>'9'||(number.ptr>key.ptr+2&&key.ptr[1]=='0'))continue;
                const auto& spec=kInputPreferenceSpecs[static_cast<unsigned>(id)];
                if(value>=spec.minimum&&value<=spec.maximum)input_[static_cast<unsigned>(id)]=value;
            }else if(count>=12&&std::memcmp(payload,"quality ",8)==0&&payload[count-1]=='\n'){
                const char* end=payload+count-1;int id=-1,value=0;
                const auto key=std::from_chars(payload+8,end,id);
                if(key.ec!=std::errc{}||key.ptr==end||*key.ptr!=' '||id<0||id>4)continue;
                const auto number=std::from_chars(key.ptr+1,end,value);
                if(number.ec!=std::errc{}||number.ptr!=end)continue;
                if(value>=0&&value<=(id==2?100:1))quality_[static_cast<unsigned>(id)]=value;
            }
        }
        return result;
    }
    std::optional<std::pair<unsigned,int>> poll_setting() noexcept {
        for(unsigned i=0;i<settings_.size();++i)if(settings_[i]){
            const int value=*settings_[i];settings_[i].reset();return std::pair<unsigned,int>{i,value};
        }
        return {};
    }
    std::optional<std::pair<unsigned,int>> poll_quality() noexcept {
        for(unsigned i=0;i<quality_.size();++i)if(quality_[i]){
            const int value=*quality_[i];quality_[i].reset();return std::pair<unsigned,int>{i,value};
        }
        return {};
    }
    bool poll_exit() noexcept {const auto value=exit_;exit_=false;return value;}
    std::optional<bool> poll_timing() noexcept {const auto result=timing_;timing_.reset();return result;}
    std::optional<bool> poll_pacing() noexcept {const auto result=pacing_;pacing_.reset();return result;}
    std::optional<bool> poll_nv12_fused() noexcept {const auto result=nv12_fused_;nv12_fused_.reset();return result;}
    std::optional<unsigned> poll_monitors() noexcept {const auto result=monitors_;monitors_.reset();return result;}
    std::optional<std::pair<unsigned,int>> poll_input() noexcept {
        for(unsigned i=0;i<input_.size();++i)if(input_[i]){
            const auto value=*input_[i];input_[i].reset();return std::pair<unsigned,int>{i,value};}
        return {};
    }
private:
    bool exit_=false;
    int fd_=-1;std::string path_;
    std::array<std::optional<int>,kRemoteSettingCount> settings_{};
    std::array<std::optional<int>,5> quality_{};
    std::optional<bool> timing_;
    std::optional<bool> pacing_;
    std::optional<bool> nv12_fused_;
    std::optional<unsigned> monitors_;
    std::array<std::optional<int>,kInputPreferenceCount> input_{};
};
// Numeric copies produced by the media worker. Packet cadence is measured at
// the decoder callback, after record parsing/metadata demux and before decode;
// it is not network arrival time. Intervals reset across media configurations.

} // namespace surfxr
