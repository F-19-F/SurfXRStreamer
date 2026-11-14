#include "audio.hpp"
#include "../desktop/diagnostics.hpp"
#include <objbase.h>
namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
using Clock=std::chrono::steady_clock;
AudioSession::AudioSession(Bytes config,AudioConfig select,MicrophoneRun run,std::function<bool(const Bytes&)> send,std::function<Bytes()> refresh):config_(std::move(config)){
    wchar_t endpoint[4096]{};const auto flags=select(config_.data(),config_.size(),endpoint,4096);flags_=flags&3;media_aes_=(flags&16)!=0;
    if(flags_&1)capture_=std::thread([this,select,send,refresh]{
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        auto next_error=Clock::now();
        while(!stop_.load()){
            try{
                const auto config=refresh();wchar_t endpoint[4096]{};const auto flags=select(config.data(),config.size(),endpoint,4096);require(flags&4,"daemon_audio_endpoint_unavailable");Audio audio(endpoint,flags&8);
                while(!stop_.load()){const auto allowed=playback_allowed_.load();audio.mute(allowed);auto pcm=audio.poll();if(allowed&&!pcm.empty())require(send(pcm),"daemon_audio_send_failed");Sleep(2);}
            }catch(const std::exception& error){if(Clock::now()>=next_error){report_failure(error,"daemon_audio_capture_failed");next_error=Clock::now()+std::chrono::seconds(5);}for(unsigned i=0;i<50&&!stop_.load();++i)Sleep(20);}
        }
        if(SUCCEEDED(com))CoUninitialize();
    });
    if(flags_&2)microphone_=std::thread([this,run]{
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);AudioCallbacks callbacks;callbacks.owner=this;
        callbacks.stopped=[](void* p){return static_cast<AudioSession*>(p)->stop_.load();};
        callbacks.receive=[](void* p,std::uint8_t* bytes,std::size_t size,bool* lost){return static_cast<AudioSession*>(p)->receive(bytes,size,lost);};
        run(config_.data(),config_.size(),&callbacks);if(SUCCEEDED(com))CoUninitialize();
    });
}
AudioSession::~AudioSession(){stop_.store(true);ready_.notify_all();if(capture_.joinable())capture_.join();if(microphone_.joinable())microphone_.join();}
void AudioSession::microphone(Bytes bytes,bool lost){
    require(bytes.size()%2==0&&bytes.size()<=192000,"daemon_microphone_pcm_bound");std::lock_guard<std::mutex> lock(mutex_);
    if(packets_.size()>=10){packets_.clear();lost=true;}packets_.emplace_back(std::move(bytes),lost);ready_.notify_one();
}
int AudioSession::receive(std::uint8_t* bytes,std::size_t size,bool* lost){
    std::unique_lock<std::mutex> lock(mutex_);ready_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop_.load()||!packets_.empty();});
    if(stop_.load()||packets_.empty())return 0;auto value=std::move(packets_.front());packets_.pop_front();if(value.first.size()>size)return -1;
    std::memcpy(bytes,value.first.data(),value.first.size());*lost=value.second;return static_cast<int>(value.first.size());
}
}
