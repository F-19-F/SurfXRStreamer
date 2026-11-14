#include "priority.hpp"
#include "surfxr/haptics.hpp"
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace surfxr::transport {
PriorityReceiver::PriorityReceiver(Connection& connection,const SessionOptions& options,
        const SessionCallbacks& callbacks,bool game_audio,bool video_haptics,std::atomic<std::uint64_t>& audio_bytes,ReceiveCounters& receive):
        wake_fd_(::eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC)){
    require(wake_fd_>=0,"surfxr_priority_wake_failed");
    try{thread_=std::thread([&,game_audio,video_haptics]{
        try{
            // Reject video and oversized PCM at the prefix, before allocation.
            const auto streams=(1u<<2)|(video_haptics?0:1u<<1);
            TcpPacketFramer framer(192000,streams);
            Reassembler assembler(connection.packet_size,true,192000,streams,connection.tcp?0:2'000'000);
            std::array<std::uint8_t,65536> buffer{};
            Bytes batch(32*connection.packet_size);std::uint64_t reordered=0,late_shards=0,reassembly_drops=0;
            const auto on_packet=[&](Packet packet){
                if(packet.stream==2){
                    require(packet.bytes.size()%4==0,"surfxr_invalid_audio_packet");
                    receive.audio.packet(now_ns(),packet.loss);
                    if(game_audio){audio_bytes.fetch_add(packet.bytes.size());
                        if(callbacks.audio_pcm)callbacks.audio_pcm(packet.bytes.data(),packet.bytes.size());}
                }else{
                    const auto pulse=haptic(packet.bytes,true);
                    if(options.haptics_queue)options.haptics_queue->ingest_pulse(pulse.hand,pulse.amplitude,pulse.duration_ns,pulse.frequency,now_ns());
                }
            };
            while(!stop_.load()&&!connection.cancelled()){
                if(connection.tcp)for(unsigned i=0;i<16&&!stop_.load();++i){
                    const auto n=connection.priority_socket.read(buffer.data(),buffer.size(),true);if(!n)break;framer.feed(buffer.data(),n,on_packet);
                }else{
                    connection.priority_socket.read_datagrams(batch.data(),connection.packet_size,32,[&](const auto* p,std::size_t n){
                        ++receive.audio.datagrams;receive.audio.bytes.fetch_add(n,std::memory_order_relaxed);
                        auto packet=assembler.accept(p,n,now_ns());if(packet)on_packet(std::move(*packet));});
                    while(auto packet=assembler.pop_ready(now_ns()))on_packet(std::move(*packet));
                    receive.audio.reordered.fetch_add(assembler.reordered()-reordered,std::memory_order_relaxed);reordered=assembler.reordered();
                    receive.audio.late_shards.fetch_add(assembler.late_shards()-late_shards,std::memory_order_relaxed);late_shards=assembler.late_shards();
                    receive.audio.reassembly_drops.fetch_add(assembler.dropped()-reassembly_drops,std::memory_order_relaxed);reassembly_drops=assembler.dropped();
                }
                const auto now=now_ns();const auto deadline=connection.tcp?std::optional<std::uint64_t>{}:assembler.next_ready_ns(now);
                const int timeout=deadline?static_cast<int>(std::min<std::uint64_t>(20,*deadline>now?(*deadline-now+999999)/1000000:0)):20;
                pollfd fds[2]{{connection.priority_socket.fd(),POLLIN,0},{wake_fd_,POLLIN,0}};::poll(fds,2,timeout);
                if(fds[1].revents)break;
            }
        }catch(...){std::lock_guard<std::mutex> lock(mutex_);error_=std::current_exception();}
    });}catch(...){::close(wake_fd_);throw;}
}
PriorityReceiver::~PriorityReceiver(){stop_.store(true);const std::uint64_t value=1;
    while(::write(wake_fd_,&value,sizeof(value))<0&&errno==EINTR){}
    if(thread_.joinable())thread_.join();
    ::close(wake_fd_);
}
void PriorityReceiver::check(){std::lock_guard<std::mutex> lock(mutex_);if(error_)std::rethrow_exception(error_);}
}
