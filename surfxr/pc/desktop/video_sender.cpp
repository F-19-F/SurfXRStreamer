#include "video_sender.hpp"
#include "pacing.hpp"
#include "video_metrics.hpp"
#include "surfxr/surfxr_stream.hpp"
#include <algorithm>

namespace surfxr::transport::desktop {
VideoSender::VideoSender(const Key& key,unsigned bitrate,State& state,bool multi,std::function<void(const Bytes&)> send,bool encrypt):
    state_(state),bitrate_(bitrate),multi_(multi),encrypt_(encrypt),key_(key),send_(std::move(send)),thread_(&VideoSender::run,this){}
VideoSender::~VideoSender(){closing_.store(true);ready_.notify_all();if(thread_.joinable())thread_.join();SecureZeroMemory(key_.data(),key_.size());}
bool VideoSender::submit(VideoBatch batch){
    require(batch.count>=1&&batch.count<=4,"desktop_video_batch_bound");
    std::unique_lock<std::mutex> lock(mutex_);
    while(pending_&&!stopped())ready_.wait_for(lock,std::chrono::milliseconds(5));
    if(stopped())return false;
    pending_=std::move(batch);ready_.notify_all();return true;
}
void VideoSender::drain(){std::unique_lock<std::mutex> lock(mutex_);while((pending_||active_)&&!stopped())ready_.wait_for(lock,std::chrono::milliseconds(5));}
void VideoSender::discard(){
    std::unique_lock<std::mutex> lock(mutex_);++epoch_;pending_.reset();ready_.notify_all();
    // Cancel the current batch before publishing another desktop. No cached
    // picture is sent during secure-desktop reconstruction.
    while(active_&&!stopped())ready_.wait_for(lock,std::chrono::milliseconds(5));
}
void VideoSender::run(){
    try{
        auto cipher=encrypt_?std::make_unique<Cipher>(key_):nullptr;VideoDeadline deadline;std::array<std::uint32_t,4> indices{};
        VideoMetrics metrics("desktop_video_sender_timing");unsigned generation=0;
        while(!stopped()){
            VideoBatch batch;std::uint64_t epoch=0;
            {std::unique_lock<std::mutex> lock(mutex_);while(!pending_&&!stopped())ready_.wait_for(lock,std::chrono::milliseconds(5));
                if(stopped())break;batch=std::move(*pending_);pending_.reset();active_=true;epoch=epoch_.load();ready_.notify_all();}
            if(generation!=batch.generation){generation=batch.generation;metrics.reset(generation);}
            std::array<std::vector<Bytes>,4> fragments;std::array<std::size_t,4> positions{};
            auto work=Clock::now();
            for(unsigned id=0;id<batch.count;++id)if(batch.pictures[id]){const auto& encoded=*batch.pictures[id];
                const auto picture=multi_?video(batch.generation,encoded.timestamp,encoded.idr,encoded.bytes.data(),encoded.bytes.size()):video(encoded.timestamp,encoded.idr,encoded.bytes.data(),encoded.bytes.size());
                fragments[id]=surfxr::transport::shards(multi_?video_streams[id]:3,indices[id]++,picture,packet_size,true);
            }
            metrics.time(0,work);const auto started=Clock::now();std::size_t sent=0;
            bool remaining=true;
            while(remaining&&!stopped()&&epoch==epoch_.load()){
                remaining=false;
                // Interleave bounded groups under the combined link budget;
                // one 4K IDR must not hold every secondary picture.
                for(unsigned id=0;id<batch.count&&!stopped()&&epoch==epoch_.load();++id){
                    work=Clock::now();unsigned group=0;
                    while(positions[id]<fragments[id].size()&&group++<4){const auto& shard=fragments[id][positions[id]++];if(cipher){auto record=cipher->seal(shard);send_(record);sent+=record.size();}else{send_(shard);sent+=shard.size()+28;}}
                    metrics.time(1,work);remaining=remaining||positions[id]<fragments[id].size();
                    if(group){work=Clock::now();deadline.wait(started+std::chrono::nanoseconds(sent*8*1000/(std::uint64_t{bitrate_}*batch.count*2)));metrics.time(2,work);}
                }
            }
            if(!stopped()&&epoch==epoch_.load()){for(unsigned id=0;id<batch.count;++id)if(batch.pictures[id])metrics.frame(id);metrics.batch();}
            {std::lock_guard<std::mutex> lock(mutex_);active_=false;ready_.notify_all();}
        }
    }catch(...){std::lock_guard<std::mutex> lock(state_.mutex);state_.error=std::current_exception();state_.stop.store(true);state_.ready.notify_all();}
    {std::lock_guard<std::mutex> lock(mutex_);active_=false;pending_.reset();ready_.notify_all();}
}
void video_sender_self_test(){
    Key key{};State state;Cipher receiver(key);surfxr::transport::Reassembler reassembler(packet_size,true);
    std::vector<unsigned> order;std::array<unsigned,4> received{};
    VideoSender sender(key,40,state,true,[&](const Bytes& record){auto plain=receiver.open(record,false);Reader header(plain,true);order.push_back(static_cast<unsigned>(header.integer(2)));
        if(auto packet=reassembler.accept(plain,1)){Reader picture(packet->bytes,true);require(picture.integer(4)==1,"desktop_sender_generation");++received[packet->stream==3?0:packet->stream==0?1:2];}
    });
    for(unsigned n=0;n<3;++n){VideoBatch batch;batch.count=3;
        for(unsigned id=0;id<3;++id)batch.pictures[id]=Encoded{Bytes(8000,static_cast<std::uint8_t>(id+n)),n==0,n+1};
        require(sender.submit(std::move(batch)),"desktop_sender_submit");
    }
    sender.drain();require(!state.stop.load()&&received[0]==3&&received[1]==3&&received[2]==3,"desktop_sender_delivery");
    require(order.size()>12&&order[0]==3&&order[4]==0&&order[8]==1,"desktop_sender_fair_groups");
    state.stop.store(true);require(!sender.submit({}),"desktop_sender_stop_backpressure");
    State plain_state;surfxr::transport::Reassembler plain_packets(packet_size,true);unsigned plain_count=0;
    VideoSender plain(key,40,plain_state,false,[&](const Bytes& bytes){if(auto packet=plain_packets.accept(bytes,1)){require(packet->stream==3,"desktop_module_plain_stream");++plain_count;}},false);
    VideoBatch batch;batch.count=1;batch.pictures[0]=Encoded{Bytes(8000,17),true,1};require(plain.submit(std::move(batch)),"desktop_module_plain_submit");plain.drain();require(!plain_state.stop.load()&&plain_count==1,"desktop_module_plain_delivery");
}
}
