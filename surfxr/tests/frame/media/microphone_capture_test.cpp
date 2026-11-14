#include "surfxr/microphone_capture.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require(rejected,"expected_rejection");}
void self_test(){
    rejects([]{surfxr::MicrophonePcmRing invalid(0);});rejects([]{surfxr::MicrophonePcmRing invalid(4801);});
    require(surfxr::microphone_source_allowed(false,"Audio/Source",nullptr),"hardware_capture_rejected");
    require(surfxr::microphone_source_allowed(false,nullptr,nullptr),"native_pulse_capture_rejected");
    require(!surfxr::microphone_source_allowed(true,"Audio/Source",nullptr),"pulse_monitor_allowed");
    require(!surfxr::microphone_source_allowed(false,"Audio/Sink",nullptr),"pipewire_sink_allowed");
    require(!surfxr::microphone_source_allowed(false,"Audio/Source","monitor"),"monitor_property_allowed");
    surfxr::MicrophonePcmRing ring(4);std::array<std::uint8_t,16> out{};
    const std::array<std::uint8_t,8> samples{0,0,1,0,0,0x80,0xff,0x7f};
    require(ring.read(out.data(),2)==0,"empty_not_empty");
    for(std::size_t i=0;i<samples.size();++i){
        require(ring.submit(&samples[i],1),"fragment_submit_failed");
        if(i<samples.size()-1)require(ring.read(out.data(),8)==0,"partial_chunk_exposed");
    }
    require(ring.read(out.data(),8)==8 && std::equal(samples.begin(),samples.end(),out.begin()),"fragment_join_failed");
    auto stats=ring.statistics();
    require(stats.capture_frames==4 && stats.consumed_frames==4 && stats.nonzero_samples==3 && stats.peak==32768
            && stats.sample_square_sum==2147418114ULL,"sample_statistics_failed");
    const std::array<std::uint8_t,8> next{2,0,3,0,4,0,5,0};
    ring.submit(samples.data(),6);require(ring.read(out.data(),4)==4,"wrap_initial_read_failed");
    ring.submit(next.data(),next.size());require(ring.read(out.data(),8)==8 && std::equal(next.begin(),next.end(),out.begin()),"oldest_drop_failed");
    require(ring.statistics().dropped_old_frames==1,"drop_statistics_failed");
    surfxr::MicrophonePcmRing overflow(2);overflow.submit(samples.data(),samples.size());
    require(overflow.read(out.data(),4)==4 && std::equal(samples.begin()+4,samples.end(),out.begin()),"oversized_keeps_recent_failed");
    require(overflow.statistics().dropped_old_frames==2,"oversized_drop_count_failed");
    require(!overflow.submit(nullptr,1) && !overflow.submit(samples.data(),9601),"invalid_chunk_allowed");
    require(overflow.read(out.data(),3)==0 && overflow.read(nullptr,2)==0 && overflow.statistics().invalid_chunks==2,"invalid_buffer_check_failed");
    ring.submit(samples.data(),7);ring.stop();ring.stop();
    require(!ring.submit(samples.data(),2) && ring.read(out.data(),2)==0 && ring.statistics().queued_frames==0,"stop_did_not_discard");
    // Deterministic patterned sample pairs catch torn samples under concurrent
    // try_lock/drop pressure; this allocates no audio device or Pulse context.
    surfxr::MicrophonePcmRing concurrent(128);std::atomic_bool done{false};std::atomic_bool valid{true};
    std::thread producer([&]{std::array<std::uint8_t,128> bytes{};for(unsigned n=0;n<10000;++n){
        for(std::size_t i=0;i<bytes.size();i+=2){bytes[i]=static_cast<std::uint8_t>(n);bytes[i+1]=static_cast<std::uint8_t>(~n);}
        concurrent.submit(bytes.data(),bytes.size());}done.store(true);});
    std::thread consumer([&]{std::array<std::uint8_t,64> bytes{};while(!done.load()){
        if(concurrent.read(bytes.data(),bytes.size()))for(std::size_t i=0;i<bytes.size();i+=2)
            if(static_cast<std::uint8_t>(bytes[i]^bytes[i+1])!=0xff)valid.store(false);
    }});producer.join();consumer.join();concurrent.stop();
    require(valid.load() && concurrent.statistics().max_queued_frames<=128,"concurrent_sample_integrity_failed");
    // Construction, read-before-start and repeated stop never connect to Pulse.
    surfxr::MicrophoneCapture capture;std::array<std::uint8_t,1920> packet{};
    require(capture.read_pcm(packet.data(),packet.size())==0,"unstarted_capture_returned_audio");
    capture.stop();capture.stop();rejects([&]{capture.start();});
    const auto state=capture.statistics();require(!state.initialized && state.stopped && !state.source_validated,"inactive_capture_state_failed");
}
}
int main(int argc,char** argv){
    try{require(argc==2 && std::strcmp(argv[1],"--self-test")==0,"only_self_test_supported");self_test();
        std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":9,\"network_used\":false,\"capture_started\":false}\n";return 0;
    }catch(const std::exception& error){std::cerr<<"{\"status\":\"self_test_failed\",\"capture_started\":false,\"error\":\""<<error.what()<<"\"}\n";return 1;}
}
