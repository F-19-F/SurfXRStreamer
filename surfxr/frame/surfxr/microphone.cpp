#include "microphone.hpp"
#include <cmath>

namespace surfxr::transport {
MicrophoneUpload::MicrophoneUpload(Connection& c,const SessionOptions& o,const SessionCallbacks& cb,MicrophoneUploadStats& stats)
    :connection_(c),options_(o),callbacks_(cb),stats_(stats),worker_([this]{run();}){}
MicrophoneUpload::~MicrophoneUpload(){stopping_.store(true);if(worker_.joinable())worker_.join();}
void MicrophoneUpload::check(){std::lock_guard<std::mutex> lock(error_mutex_);if(error_)std::rethrow_exception(error_);}
bool MicrophoneUpload::cancelled() const {return stopping_.load()||(connection_.cancelled&&connection_.cancelled());}
void MicrophoneUpload::run(){
    bool attempted=false,active=false;
    const auto stop_capture=[&]{
        if(attempted){attempted=false;try{callbacks_.microphone_stop();}catch(...){++stats_.capture_failures;}}
        active=false;
    };
    auto retry_at=Clock::now();
    try{
        while(!cancelled()){
            if(!active&&Clock::now()>=retry_at){
                attempted=true;++stats_.start_attempts;
                try{active=callbacks_.microphone_start();}catch(...){active=false;}
                if(!active){++stats_.capture_failures;stop_capture();retry_at=Clock::now()+std::chrono::seconds(1);}
            }
            std::array<std::uint8_t,1920> pcm{};std::size_t size=0;
            if(active)try{
                size=callbacks_.microphone_pcm(pcm.data(),pcm.size());
                require(size==0||size==pcm.size(),"surfxr_invalid_microphone_packet");
                if(size){
                    const auto gain=callbacks_.microphone_volume?callbacks_.microphone_volume():static_cast<float>(options_.initial_microphone_volume);
                    require(std::isfinite(gain)&&gain>=0&&gain<=1,"surfxr_invalid_microphone_gain");
                    for(std::size_t i=0;i<pcm.size();i+=2){
                        const auto raw=std::uint16_t(pcm[i])|(std::uint16_t(pcm[i+1])<<8);
                        const auto value=static_cast<std::int16_t>(std::lround(static_cast<std::int16_t>(raw)*gain));
                        pcm[i]=static_cast<std::uint8_t>(value);pcm[i+1]=static_cast<std::uint8_t>(std::uint16_t(value)>>8);
                    }
                }
            }catch(...){
                ++stats_.capture_failures;stop_capture();size=0;retry_at=Clock::now()+std::chrono::seconds(1);
            }
            // Socket errors belong to connection recovery, not local capture
            // recovery: propagate them instead of silently disabling upload.
            if(size){connection_.send_stream(2,Bytes(pcm.begin(),pcm.end()));++stats_.packets;}
            // At most one 20 ms packet per iteration; drain a finite capture
            // backlog without a busy loop. Empty/muted capture sends no packet.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }catch(const Cancelled&){}
    catch(...){std::lock_guard<std::mutex> lock(error_mutex_);error_=std::current_exception();}
    stop_capture();
}
}
