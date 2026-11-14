#include "../../pc/desktop/video_sender.hpp"
#include "../../pc/desktop/pacing.hpp"
#include <future>
#include <iostream>

namespace {
using namespace surfxr::transport::desktop;
using surfxr::transport::Bytes;
using surfxr::transport::require;
VideoBatch picture(unsigned generation=1){VideoBatch batch;batch.generation=generation;batch.pictures[0]=Encoded{Bytes(generation==1?25000:1,17),true,generation};return batch;}
struct Gate {
    std::mutex mutex;std::condition_variable ready;bool entered=false,released=false;
    void block(){std::unique_lock<std::mutex> lock(mutex);entered=true;ready.notify_all();ready.wait(lock,[&]{return released;});}
    void wait(){std::unique_lock<std::mutex> lock(mutex);require(ready.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}),"sender_test_gate_timeout");}
    void release(){std::lock_guard<std::mutex> lock(mutex);released=true;ready.notify_all();}
    ~Gate(){release();}
};
void discarded_desktop(){
    Key key{};State state;Gate gate;std::atomic_uint records{0};
    VideoSender sender(key,40,state,true,[&](const Bytes&){if(records.fetch_add(1)==0)gate.block();});
    struct Release {Gate& gate;~Release(){gate.release();}} release{gate};
    require(sender.submit(picture()),"sender_test_first_submit");gate.wait();
    require(sender.submit(picture()),"sender_test_pending_submit");
    auto cancelled=std::async(std::launch::async,[&]{sender.discard();});
    require(cancelled.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"sender_discard_skipped_active_barrier");
    gate.release();require(cancelled.wait_for(std::chrono::seconds(2))==std::future_status::ready,"sender_discard_deadlocked");cancelled.get();
    const auto old=records.load();require(sender.submit(picture(2)),"sender_new_desktop_submit");sender.drain();
    require(!state.stop.load()&&records.load()==old+1,"sender_retained_old_desktop_batch");
}
void stop_backpressure(){
    Key key{};State state;Gate gate;
    VideoSender sender(key,40,state,true,[&](const Bytes&){gate.block();});
    struct Release {Gate& gate;~Release(){gate.release();}} release{gate};
    require(sender.submit(picture()),"sender_test_first_submit");gate.wait();
    require(sender.submit(picture()),"sender_test_pending_submit");
    auto blocked=std::async(std::launch::async,[&]{return sender.submit(picture());});
    require(blocked.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"sender_pending_queue_unbounded");
    state.stop.store(true);
    require(blocked.wait_for(std::chrono::seconds(2))==std::future_status::ready&&!blocked.get(),"sender_stop_did_not_release_producer");gate.release();
}
void send_failure(){
    Key key{};State state;VideoSender sender(key,40,state,true,[](const Bytes&){throw std::runtime_error("synthetic_send_failure");});
    require(sender.submit(picture()),"sender_test_first_submit");sender.drain();
    std::lock_guard<std::mutex> lock(state.mutex);require(state.stop.load()&&bool(state.error),"sender_error_not_propagated");
}
}
int main(){try{
    discarded_desktop();stop_backpressure();send_failure();
    surfxr::transport::desktop::VideoDeadline deadline;double total=0;
    for(unsigned i=0;i<24;++i){const auto start=Clock::now();deadline.wait(start+std::chrono::microseconds(500));total+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
    std::cout<<"Desktop production sender: bounded backpressure, desktop discard barrier, stop and send failure passed; 500 us kernel-wait mean_ms="<<total/24<<". No capture, input, events or network.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
