#include "surfxr/vr_wire.hpp"
namespace surfxr {
bool SessionControlQueue::add_monitor(){return push(SessionCommand::AddMonitor);}
bool SessionControlQueue::remove_monitor(){return push(SessionCommand::RemoveMonitor);}
bool SessionControlQueue::force_keyframe(){return push(SessionCommand::Keyframe);}
bool SessionControlQueue::restart_stream(){return push(SessionCommand::Restart);}
bool SessionControlQueue::push(SessionCommand type){
    std::lock_guard<std::mutex> lock(mutex_);
    if(queue_.size()>=64){++stats_.rejected;return false;}
    queue_.push_back({type});++stats_.accepted;return true;
}
bool SessionControlQueue::pop(SessionControlCommand& command){
    std::lock_guard<std::mutex> lock(mutex_);if(queue_.empty())return false;
    command=queue_.front();queue_.pop_front();++stats_.dequeued;return true;
}
SessionControlStats SessionControlQueue::statistics()const{std::lock_guard<std::mutex> lock(mutex_);return stats_;}
}
