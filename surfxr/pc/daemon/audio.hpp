#pragma once
#include "../desktop/audio.hpp"
#include <deque>
#include <condition_variable>
#include <thread>
namespace surfxr::daemon {
struct AudioCallbacks {
    void* owner=nullptr;bool (*stopped)(void*)=nullptr;
    int (*receive)(void*,std::uint8_t*,std::size_t,bool*)=nullptr;
};
using AudioConfig=unsigned (*)(const std::uint8_t*,std::size_t,wchar_t*,std::size_t);
using MicrophoneRun=int (*)(const std::uint8_t*,std::size_t,const AudioCallbacks*);
class AudioSession {
public:
    AudioSession(surfxr::transport::Bytes,AudioConfig,MicrophoneRun,std::function<bool(const surfxr::transport::Bytes&)>,std::function<surfxr::transport::Bytes()> refresh);
    ~AudioSession();
    unsigned flags()const{return flags_;}
    bool media_aes()const{return media_aes_;}
    void microphone(surfxr::transport::Bytes,bool);
    void playback_allowed(bool allowed){playback_allowed_.store(allowed);}
private:
    int receive(std::uint8_t*,std::size_t,bool*);
    surfxr::transport::Bytes config_;unsigned flags_=0;bool media_aes_=false;
    std::atomic_bool stop_{false},playback_allowed_{true};
    std::mutex mutex_;std::condition_variable ready_;std::deque<std::pair<surfxr::transport::Bytes,bool>> packets_;
    std::thread capture_,microphone_;
};
}
