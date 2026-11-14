#pragma once
#include "video.hpp"
#include <optional>
#include <functional>
#include <thread>

namespace surfxr::transport::desktop {
struct VideoBatch {
    unsigned generation=1,count=1;
    std::array<std::optional<Encoded>,4> pictures;
};
// One pending batch plus one sending batch. Backpressure preserves all codec
// references; no GPU resources cross threads and no unbounded video FIFO forms.
class VideoSender {
public:
    VideoSender(const Key&,unsigned bitrate,State&,bool multi,std::function<void(const Bytes&)> send,bool encrypt=true);
    ~VideoSender();
    bool submit(VideoBatch);
    void discard();
    void drain();
private:
    State& state_;unsigned bitrate_;bool multi_,encrypt_;Key key_;
    std::function<void(const Bytes&)> send_;
    std::mutex mutex_;std::condition_variable ready_;std::optional<VideoBatch> pending_;bool active_=false;
    std::atomic_bool closing_{false};std::atomic_uint64_t epoch_{0};std::thread thread_;
    void run();
    bool stopped()const{return closing_.load()||state_.stop.load();}
};
void video_sender_self_test();
}
