#include "surfxr/audio_output.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<std::uint8_t> tone(unsigned frames,std::int16_t left=24000,std::int16_t right=-24000){
    std::vector<std::uint8_t> pcm(frames*4);
    for(unsigned f=0;f<frames;++f)for(unsigned c=0;c<2;++c){
        const auto value=static_cast<std::uint16_t>(c?right:left);
        pcm[f*4+c*2]=static_cast<std::uint8_t>(value);pcm[f*4+c*2+1]=static_cast<std::uint8_t>(value>>8);
    }
    return pcm;
}
int sample(const std::vector<std::uint8_t>& pcm,unsigned frame,unsigned channel){
    const auto p=frame*4+channel*2;const auto v=unsigned(pcm[p])|(unsigned(pcm[p+1])<<8);
    return v<32768?int(v):int(v)-65536;
}
unsigned buffered_checks(){
    surfxr::AudioPcmRing ring(2880,surfxr::AudioPlaybackMode::Buffered);
    auto pcm=tone(480);std::vector<std::uint8_t> output(288*4);
    require(ring.submit(pcm.data(),pcm.size()),"buffered_submit");
    require(ring.read(output.data(),output.size())==0&&ring.statistics().queued_frames==480,"prime_preserves_pcm");
    pcm=tone(384);ring.submit(pcm.data(),pcm.size());
    require(ring.read(output.data(),output.size())==output.size(),"prime_with_callback_and_reserve");
    require(sample(output,0,0)==250&&sample(output,0,1)==-250&&sample(output,95,0)==24000&&sample(output,95,1)==-24000,"signed_stereo_fade_in");
    require(ring.statistics().queued_frames==576&&ring.statistics().buffering_target_frames==576,"bounded_reserve");
    // A 12 ms delivery interruption consumes the reserve without a hole.
    for(unsigned i=0;i<2;++i){
        require(ring.read(output.data(),output.size())==output.size()&&sample(output,0,0)==24000,"reserve_absorbs_delivery_jitter");
    }
    require(ring.statistics().rebuffer_events==0&&ring.statistics().underrun_callbacks==0,"short_jitter_not_underrun");
    require(ring.read(output.data(),output.size())==0&&sample(output,0,0)==23750&&sample(output,0,1)==-23750&&sample(output,95,0)==0&&sample(output,287,1)==0,"empty_queue_tapers_to_silence");
    require(ring.statistics().rebuffer_events==1&&ring.statistics().underrun_callbacks==1,"underrun_starts_recovery_once");
    pcm=tone(480);ring.submit(pcm.data(),pcm.size());
    require(ring.read(output.data(),output.size())==0&&ring.statistics().queued_frames==480,"recovery_waits_for_reserve");
    pcm=tone(384);ring.submit(pcm.data(),pcm.size());
    require(ring.read(output.data(),output.size())==output.size()&&sample(output,0,0)==250,"recovery_fades_in");
    // Overflow must keep recent PCM, with a bounded transition at the splice.
    pcm=tone(2880,-24000,24000);ring.submit(pcm.data(),pcm.size());
    require(ring.read(output.data(),output.size())==output.size()&&sample(output,0,0)==23500&&sample(output,0,1)==-23500&&sample(output,95,0)==-24000,"overflow_crossfade");
    require(ring.statistics().dropped_old_frames==576,"overflow_remains_bounded");
    // A reset must forget PCM fragments and the old output's taper anchor.
    const std::uint8_t fragment=255;ring.submit(&fragment,1);ring.discard_pending();
    pcm=tone(864);ring.submit(pcm.data(),pcm.size());
    require(ring.read(output.data(),output.size())==output.size()&&sample(output,0,0)==250&&ring.statistics().partial_bytes==0,"reset_does_not_replay_old_audio");
    ring.stop();require(ring.read(output.data(),output.size())==0&&output==std::vector<std::uint8_t>(output.size()),"buffered_stop_silent");
    // A negotiated request larger than the ring cannot prevent all playback.
    surfxr::AudioPcmRing large(2880,surfxr::AudioPlaybackMode::Buffered);
    pcm=tone(2880,32767,-32768);large.submit(pcm.data(),pcm.size());output.resize(4800*4);
    require(large.read(output.data(),output.size())==2880*4&&sample(output,95,0)==32767&&sample(output,95,1)==-32768&&sample(output,2879,0)==0,"large_callback_bounded_and_signed");
    require(large.statistics().rebuffer_events==1&&large.statistics().queued_frames==0,"large_callback_recovers");
    return 6;
}
unsigned concurrent_buffered_check(){
    surfxr::AudioPcmRing ring(2880,surfxr::AudioPlaybackMode::Buffered);
    const auto pcm=tone(96);std::atomic_bool done{false},failed{false};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    std::thread producer([&]{
        for(unsigned i=0;i<1000&&!failed.load();++i){
            while(ring.statistics().queued_frames>1920&&!failed.load()){
                if(std::chrono::steady_clock::now()>deadline){failed.store(true);break;}
                std::this_thread::yield();
            }
            if(!ring.submit(pcm.data(),pcm.size()))failed.store(true);
        }
        done.store(true);
    });
    std::vector<std::uint8_t> output(288*4);std::uint64_t consumed=0;bool started=false;
    while(!failed.load()){
        const auto available=ring.statistics().queued_frames;
        if(done.load()&&!available)break;
        if((!started&&available<864)||!available){
            if(std::chrono::steady_clock::now()>deadline){failed.store(true);break;}
            std::this_thread::yield();continue;
        }
        const auto frames=std::min<unsigned>(available,288);const auto got=ring.read(output.data(),frames*4);
        if(got!=frames*4){failed.store(true);break;}
        for(unsigned f=0;f<frames;++f)if(consumed+f>=96&&(sample(output,f,0)!=24000||sample(output,f,1)!=-24000))failed.store(true);
        consumed+=frames;started=true;
    }
    producer.join();const auto stats=ring.statistics();
    require(!failed.load()&&consumed==96000&&stats.dropped_contention_frames==0&&stats.dropped_old_frames==0&&stats.underrun_callbacks==0,"concurrent_buffered_pcm_lost_or_torn");
    return 1;
}
unsigned self_test(){
    const std::array<std::uint8_t,20> input{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
    // Every possible fragmentation boundary reconstructs complete stereo frames.
    for(std::size_t split=0;split<=input.size();++split){
        surfxr::AudioPcmRing ring(8);std::array<std::uint8_t,20> output{};
        require(ring.submit(input.data(),split),"split_first");require(ring.submit(input.data()+split,input.size()-split),"split_second");
        require(ring.read(output.data(),output.size())==input.size() && output==input,"fragment_reconstruction");
    }
    {surfxr::AudioPcmRing ring(8);std::array<std::uint8_t,8> output{};
        for(std::size_t i=0;i<7;++i)require(ring.submit(input.data()+i,1),"byte_fragment");
        require(ring.read(output.data(),8)==4,"partial_not_consumed");
        require(std::memcmp(output.data(),input.data(),4)==0 && output[4]==0 && output[7]==0,"silence_tail");
        require(ring.statistics().partial_bytes==3 && ring.statistics().silence_frames==1,"partial_stats");
        ring.submit(input.data()+7,1);require(ring.read(output.data(),4)==4 && std::memcmp(output.data(),input.data()+4,4)==0,"partial_survives_read");}
    {surfxr::AudioPcmRing ring(3);std::array<std::uint8_t,12> output{};
        ring.submit(input.data(),8);ring.submit(input.data()+8,12);
        require(ring.read(output.data(),12)==12 && std::memcmp(output.data(),input.data()+8,12)==0,"overflow_drops_oldest");
        require(ring.statistics().dropped_old_frames==2,"overflow_count");}
    {surfxr::AudioPcmRing ring(3);std::array<std::uint8_t,12> output{};
        ring.submit(input.data(),3);ring.submit(input.data()+3,17);
        require(ring.read(output.data(),12)==12 && std::memcmp(output.data(),input.data()+8,12)==0,"large_fragment_overflow");}
    {surfxr::AudioPcmRing ring(3);std::array<std::uint8_t,12> output{};
        ring.submit(input.data(),12);ring.read(output.data(),8);ring.submit(input.data()+12,8);
        require(ring.read(output.data(),12)==12 && std::memcmp(output.data(),input.data()+8,12)==0,"ring_wrap");}
    {surfxr::AudioPcmRing ring(3);std::array<std::uint8_t,4> output{};
        ring.submit(input.data(),4);require(ring.read(output.data(),2)==0,"unaligned_read_rejected");
        require(ring.read(output.data(),4)==4 && std::memcmp(output.data(),input.data(),4)==0,"unaligned_read_preserves_sample");}
    {surfxr::AudioPcmRing ring;std::array<std::uint8_t,8> output{};
        require(!ring.submit(nullptr,1),"invalid_pointer");require(!ring.submit(input.data(),16u*1024u*1024u+1),"invalid_size");
        ring.submit(input.data(),7);ring.stop();ring.stop();require(!ring.submit(input.data(),4),"stop_rejects");
        require(ring.read(output.data(),8)==0 && output==std::array<std::uint8_t,8>{},"stop_silence");
        const auto s=ring.statistics();require(s.stopped && s.queued_frames==0 && s.partial_bytes==0 && s.invalid_submissions==2,"stop_stats");}
    {bool rejected=false;try{surfxr::AudioPcmRing bad(4801);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"capacity_limit");}
    {surfxr::AudioOutput output;require(!output.submit_pcm(input.data(),4),"uninitialized_rejects");output.stop();
        const auto stats=output.statistics();require(!stats.initialized && stats.stopped && stats.write_callbacks==0,"offline_no_audio");}
    return 9+buffered_checks()+concurrent_buffered_check()+surfxr::audio_output_recovery_self_test();
}
}
int main(int argc,char** argv){
    if(argc!=2 || std::string(argv[1])!="--self-test"){std::cout<<"surfxr-audio-test --self-test (offline only; never starts playback)\n";return argc==1?0:1;}
    try{const auto groups=self_test();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"audio_played\":false,\"network_used\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<"audio_self_test: "<<e.what()<<'\n';return 1;}
}
