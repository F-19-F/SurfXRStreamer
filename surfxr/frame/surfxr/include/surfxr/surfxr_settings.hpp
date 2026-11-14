#pragma once
#include "surfxr/surfxr_codec.hpp"
#include "surfxr/remote_settings.hpp"
#include <deque>
#include <functional>
#include <random>

namespace surfxr::transport {
// One persistent control lane, independent of desktop/VR generations. Only
// explicit UI edits enter this queue; saved client preferences never do.
class SettingsSync {
public:
    explicit SettingsSync(RemoteSettings& settings,std::function<void()> saved={}):settings_(settings),saved_(std::move(saved)){
        std::random_device random;token_=(std::uint64_t(random())<<32)|random();if(!token_)token_=1;
    }
    bool request(SettingChange change){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ready_||queue_.size()>=16||!setting_request_allowed(change,settings_.snapshot()))return false;
        const auto id=static_cast<unsigned>(change.id);const bool pointer=change.id==SettingId::DesktopPointerPriority;if(id>2&&!pointer)return false;
        queue_.push_back({++sequence_,pointer?5:id+1,change.value});settings_.request(change);return true;
    }
    bool exit(){std::lock_guard<std::mutex> lock(mutex_);if(!ready_||queue_.size()>=16)return false;queue_.push_back({++sequence_,4,0});return true;}
    Bytes poll(){
        std::lock_guard<std::mutex> lock(mutex_);const auto command=queue_.empty()?Command{}:queue_.front();
        Writer w;w.integer(token_,8);w.integer(command.sequence,8);w.integer(command.action|0x80,1);
        std::uint64_t bits;std::memcpy(&bits,&command.value,8);w.integer(bits,8);return w.bytes;
    }
    void receive(const Bytes& bytes){
        Reader r(bytes);const auto ack=r.integer(8),revision=r.integer(8);std::array<double,3> values{};
        for(auto& value:values){const auto bits=r.integer(8);std::memcpy(&value,&bits,8);require(std::isfinite(value),"surfxr_settings_nonfinite");}
        const bool pointer=r.remaining()==8;double priority=0;
        if(pointer){const auto bits=r.integer(8);std::memcpy(&priority,&bits,8);require(priority==0||priority==1,"surfxr_pointer_range");}r.end();
        require(values[0]>=1&&values[0]<=1000&&values[1]>=-2&&values[1]<=7&&values[2]>=0&&values[2]<=1000,"surfxr_settings_range");
        std::lock_guard<std::mutex> lock(mutex_);
        const bool accepted=ack&&!queue_.empty()&&queue_.front().sequence==ack;
        if(accepted)queue_.pop_front();
        if(revision<revision_)return;
        const bool changed=!ready_||revision_!=revision;
        revision_=revision;ready_=true;
        std::array<bool,3> pending{};for(const auto& command:queue_)if(command.action>=1&&command.action<=3)pending[command.action-1]=true;
        settings_.synchronize_surfxr(values,pending);
        bool pointer_pending=false;for(const auto& command:queue_)pointer_pending|=command.action==5;
        settings_.synchronize_pointer(priority,pointer_pending,pointer);
        if((changed||accepted)&&saved_&&queue_.empty())saved_();
    }
    void disconnected(){
        std::lock_guard<std::mutex> lock(mutex_);ready_=false;
        // Do not replay queued edits after a new connection. The PC persists any
        // accepted request; the next handshake reads that authoritative state.
        queue_.clear();revision_=0;settings_.synchronize_pointer(0,false,false);
    }
private:
    struct Command {std::uint64_t sequence=0;unsigned action=0;double value=0;};
    RemoteSettings& settings_;std::function<void()> saved_;std::mutex mutex_;std::deque<Command> queue_;
    std::uint64_t token_=0,sequence_=0,revision_=0;bool ready_=false;
};
}
