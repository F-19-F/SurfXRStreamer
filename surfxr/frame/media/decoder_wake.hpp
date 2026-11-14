#pragma once
// Shared lifetime for command/GPU-release notification; callbacks never borrow
// a device FD that may have been closed by configure/deactivate.
#include "surfxr/video_decoder.hpp"
#include <array>
#include <cerrno>
#ifdef __linux__
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif
namespace surfxr::decoder_detail {
class DecoderWake {
public:
    DecoderWake(){
#ifdef __linux__
        fd_=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(fd_<0)throw VideoDecodeError("decoder_wake_create_failed");
#endif
    }
    ~DecoderWake(){
#ifdef __linux__
        if(fd_>=0)close(fd_);
#endif
    }
    DecoderWake(const DecoderWake&)=delete;DecoderWake& operator=(const DecoderWake&)=delete;
    int descriptor()const noexcept{return fd_;}
    void notify()const noexcept{
#ifdef __linux__
        const std::uint64_t one=1;ssize_t result;
        do{result=write(fd_,&one,sizeof(one));}while(result<0&&errno==EINTR);
#endif
    }
    void consume()const noexcept{
#ifdef __linux__
        std::uint64_t value;ssize_t result;
        do{result=read(fd_,&value,sizeof(value));}while(result<0&&errno==EINTR);
#endif
    }
    bool wait(const std::array<int,2>& sources)const{
#ifdef __linux__
        std::array<pollfd,3> descriptors{{{fd_,POLLIN,0},{sources[0],POLLIN|POLLPRI,0},{sources[1],POLLIN,0}}};
        int result;do{result=::poll(descriptors.data(),descriptors.size(),-1);}while(result<0&&errno==EINTR);
        if(result<0)throw VideoDecodeError("decoder_wait_failed");
        for(const auto& descriptor:descriptors)if(descriptor.revents&POLLNVAL)throw VideoDecodeError("decoder_wait_invalid_descriptor");
        if(descriptors[0].revents)consume();
        return descriptors[1].revents||descriptors[2].revents;
#else
        (void)sources;return false;
#endif
    }
private:
    int fd_=-1;
};
}
