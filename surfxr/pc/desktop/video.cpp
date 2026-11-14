#include "video.hpp"
#include "diagnostics.hpp"
#include "pacing.hpp"
#include "video_sender.hpp"
#include "video_metrics.hpp"
#include "surfxr/surfxr_stream.hpp"
#include <thread>
#include <algorithm>
#include <objbase.h>

namespace surfxr::transport::desktop {
namespace {
struct Output {
    std::unique_ptr<Capture> capture;std::unique_ptr<Encoder> encoder;
    bool have_image=false,idr=true;
    Output(unsigned monitor,const Options& o){
        auto phase=Clock::now();capture=std::make_unique<Capture>(monitor);report_duration("desktop_capture_start_ms",phase);
        phase=Clock::now();encoder=std::make_unique<Encoder>(*capture,o.fps,o.bitrate);report_duration("desktop_encoder_start_ms",phase);
    }
};
Bytes mono_config(Encoder& e){Writer w(true);w.integer(1,4);w.integer(1,4);w.blob(e.configuration());w.string("");return w.bytes;}
}
void video_worker(const Options& o,Control& control,Socket media,sockaddr_in peer,const Key& key,State& state,bool multi){
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        auto monitors=capture_monitors(o.monitor);if(!multi)monitors.resize(1);
        std::vector<std::unique_ptr<Output>> outputs;
        Layout current;current.fps=o.fps;current.capacity=static_cast<unsigned>(monitors.size());
        const auto restore_outputs=[&]{
            // Keep the PC preference when displays disappear or the GPU cannot
            // open another encoder. Always retain the usable primary stream.
            const auto count=std::clamp(state.requested_count.load(),1u,current.capacity);
            while(outputs.size()<count){
                try{outputs.push_back(std::make_unique<Output>(monitors[outputs.size()],o));}
                catch(...){if(outputs.empty())throw;current.capacity=static_cast<unsigned>(outputs.size());report_event("desktop_monitor_add_failed");break;}
            }
        };
        restore_outputs();
        VideoMetrics metrics("desktop_video_producer_timing");VideoDeadline deadline;
        VideoSender sender(key,o.bitrate,state,multi,[&](const Bytes& record){
            if(o.send_video){o.send_video(record);return;}
            const auto n=sendto(media.value,reinterpret_cast<const char*>(record.data()),static_cast<int>(record.size()),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer));require(n==static_cast<int>(record.size()),"desktop_video_send_failed");
        },!o.embedded);
        const auto publish=[&](bool send){
            current.streams.clear();for(auto& output:outputs){current.streams.push_back({output->capture->display(),2,output->encoder->configuration()});output->idr=true;}
            if(send){if(multi)control.send(layout(current));else control.send(mono_config(*outputs.front()->encoder));}
            {std::lock_guard<std::mutex> lock(state.mutex);state.layout=current;state.initialized=true;state.ready.notify_one();}
            metrics.reset(current.generation);
        };
        publish(false);
        while(!state.stop.load()&&!state.stream_ready.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!state.stop.load())publish(true);
        const auto period=std::chrono::nanoseconds(1'000'000'000/o.fps);auto next=Clock::now();
        unsigned capture_mask=15;
        while(!state.stop.load()){
            next+=period;const auto requested=std::clamp(state.requested_count.load(),1u,current.capacity);
            if(requested!=outputs.size()){
                sender.discard();
                if(requested<outputs.size())outputs.resize(requested);
                else restore_outputs();
                ++current.generation;publish(true);
            }
            const auto wanted_mask=state.capture_requested.load();
            if(wanted_mask!=capture_mask){
                sender.discard();
                for(unsigned id=0;id<outputs.size();++id)if((wanted_mask&~capture_mask)&(1u<<id))outputs[id]->idr=true;
                capture_mask=wanted_mask;report_event("desktop_capture_mask_applied",capture_mask,EVENTLOG_INFORMATION_TYPE);
            }
            state.capture_applied.store(capture_mask);state.capture_generation.store(current.generation);
            const bool force=state.idr.exchange(false);
            VideoBatch batch;batch.generation=current.generation;batch.count=static_cast<unsigned>(outputs.size());bool recovered=false;
            for(unsigned id=0;id<outputs.size()&&!state.stop.load();++id){if(!(capture_mask&(1u<<id)))continue;auto& output=*outputs[id];output.idr=output.idr||force;bool fresh=false;
                auto work=Clock::now();
                try{fresh=output.capture->poll();}catch(...){
                    // Retire every cached output on desktop access-loss. Each
                    // encoder closes before its GPU capture/fence owner.
                    sender.discard();recovered=true;
                    outputs.clear();
                    const auto recovery_deadline=Clock::now()+std::chrono::seconds(10);
                    while(!state.stop.load()){
                        try{monitors=capture_monitors(o.monitor);if(!multi)monitors.resize(1);
                            current.capacity=static_cast<unsigned>(monitors.size());restore_outputs();break;}
                        catch(...){outputs.clear();require(Clock::now()<recovery_deadline,"desktop_capture_recovery_failed_service_required");std::this_thread::sleep_for(std::chrono::milliseconds(50));}
                    }
                    if(!state.stop.load()){++current.generation;publish(true);}break;
                }
                output.have_image=output.have_image||fresh;if(!output.have_image)continue;
                output.capture->compose_cursor();
                metrics.time(0,work);work=Clock::now();
                const auto stamp=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
                auto encoded=output.encoder->encode(*output.capture,output.idr,stamp);output.idr=false;
                metrics.time(1,work);metrics.frame(id);batch.pictures[id]=std::move(encoded);
            }
            if(!recovered&&capture_mask){const auto work=Clock::now();if(!sender.submit(std::move(batch)))break;metrics.time(2,work);metrics.batch();}
            if(next<Clock::now()-period)next=Clock::now();deadline.wait(next);
        }
    }catch(...){std::lock_guard<std::mutex> lock(state.mutex);state.error=std::current_exception();state.ready.notify_one();state.stop.store(true);}
    if(SUCCEEDED(com))CoUninitialize();
}
}
