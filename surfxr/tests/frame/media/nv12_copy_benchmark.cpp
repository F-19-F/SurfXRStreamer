// Explicit hardware benchmark: borrowed Iris CAPTURE -> the production Vulkan
// NV12 storage. Copies finish inside receive(), before the original QBUF.
#include "../../../frame/media/v4l2_hevc.hpp"
#include "../../../frame/media/nv12_output.hpp"
#include "nv12_copy_candidates.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"
#include "../xr/vulkan_fixture.hpp"
extern "C" {
#include <libavformat/avformat.h>
}
#include <chrono>
#include <iostream>
#include "nv12_dmabuf_check.hpp"

namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const char* label){if(!value)throw surfxr::VideoDecodeError(label);}
void libc_copy(std::uint8_t* out,const std::uint8_t* in,std::size_t bytes){std::memcpy(out,in,bytes);}
struct Format {AVFormatContext* value=nullptr;~Format(){avformat_close_input(&value);}};
struct Packet {AVPacket* value=av_packet_alloc();~Packet(){av_packet_free(&value);}};
struct Frame {AVFrame* value=av_frame_alloc();~Frame(){av_frame_free(&value);}};
struct Result {
    const char* name;
    surfxr::decoder_detail::Nv12PlaneCopy copy;
    std::vector<std::uint64_t> ns;
};
void verify(const AVFrame& frame,surfxr::Nv12FrameStorage& storage){
    for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?storage.height/2:storage.height);++row)
        require(std::memcmp(storage.data(p)+row*storage.stride[p],frame.data[p]+row*frame.linesize[p],storage.width)==0,
            "nv12_hardware_copy_bytes_differ");
}
}

void nv12_copy_candidates_self_test(){
    // Alignments, thresholds, vector/prefetch tails and surrounding guards.
    constexpr std::size_t capacity=131200;
    std::vector<std::uint8_t> source(capacity+256),destination(capacity+256);
    auto* in=source.data()+128-(reinterpret_cast<std::uintptr_t>(source.data())&63);
    auto* out=destination.data()+128-(reinterpret_cast<std::uintptr_t>(destination.data())&63);
    for(std::size_t i=0;i<capacity;++i)in[i]=std::uint8_t(i*17+i/127);
    for(auto copy:{surfxr::nv12_test::copy_nv12_plane_arm64<false>,surfxr::nv12_test::copy_nv12_plane_arm64<true>,
                  surfxr::nv12_test::copy_nv12_plane_arm64<true,true>})
        for(auto bytes:{0u,1u,127u,128u,129u,65535u,65536u,65537u,65663u,65664u,131071u})
            for(unsigned offset:{0u,1u,15u,16u,63u}){
                std::fill(destination.begin(),destination.end(),0xcd);copy(out+offset,in+offset,bytes);
                require(std::memcmp(out+offset,in+offset,bytes)==0&&(out+offset)[-1]==0xcd&&out[offset+bytes]==0xcd,
                    "nv12_candidate_bytes_or_guard_corrupt");
            }
}

int nv12_copy_benchmark(const std::string& input,unsigned width,unsigned height){
    surfxr::gpu_test::Vulkan gpu;gpu.initialize(false,VK_API_VERSION_1_1,true);
    std::array<std::shared_ptr<surfxr::Nv12FrameStorage>,6> destinations;
    for(auto& destination:destinations){
        destination=surfxr::VulkanNv12FrameStorage::allocate(gpu.physical,gpu.device,width,height);
        require(bool(destination),"nv12_benchmark_vulkan_storage_unavailable");
    }
    std::array<Result,4> results{{
        {"libc",libc_copy,{}},
        {"arm64_cached",surfxr::nv12_test::copy_nv12_plane_arm64<false>,{}},
        {"arm64_streaming",surfxr::nv12_test::copy_nv12_plane_arm64<true>,{}},
        {"arm64_streaming_prefetch",surfxr::nv12_test::copy_nv12_plane_arm64<true,true>,{}}
    }};
    Format format;AVDictionary* options=nullptr;av_dict_set(&options,"protocol_whitelist","file",0);
    const int opened=avformat_open_input(&format.value,input.c_str(),av_find_input_format("hevc"),&options);
    av_dict_free(&options);require(opened>=0,"nv12_benchmark_open_failed");
    Packet packet;Frame frame;require(packet.value&&frame.value,"nv12_benchmark_allocation_failed");
    surfxr::nv12_test::DmaBufCheck dmabuf(gpu);
    auto decoder_owner=std::make_unique<surfxr::V4l2Hevc>(width,height,true);auto& decoder=*decoder_owner;
    unsigned packets=0,frames=0;std::array<int,2> source_stride{};int export_result=AVERROR(EAGAIN);
    const auto consume=[&]{
        source_stride={frame.value->linesize[0],frame.value->linesize[1]};
        if(!frames)export_result=decoder.probe_capture_dmabuf_export();
        // Warm each method equally, rotate both block order and the six image
        // owners. Validation is outside the timer; no private pixels are saved.
        for(unsigned method=0;method<results.size();++method)
            surfxr::decoder_detail::write_nv12(*frame.value,*destinations[method],results[method].copy);
        for(unsigned block=0;block<results.size();++block)for(unsigned order=0;order<results.size();++order){
            auto& result=results[(frames+block+order)%results.size()];
            for(unsigned repetition=0;repetition<8;++repetition){
                auto& destination=*destinations[(repetition+frames+block)%6];
                const auto begin=Clock::now();
                surfxr::decoder_detail::write_nv12(*frame.value,destination,result.copy);
                const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
                if(frames>=2)result.ns.push_back(ns);
            }
            verify(*frame.value,*destinations[(7+frames+block)%6]);
        }
        dmabuf.check(decoder,*frame.value,*destinations[0]);
        ++frames;
    };
    const auto receive=[&]{return decoder.receive(frame.value,consume);};
    const auto checked_receive=[&]{
        const int result=receive();
        require(result>=0||result==AVERROR(EAGAIN)||result==AVERROR_EOF,"nv12_benchmark_receive_failed");
        return result;
    };
    const auto submit=[&](const AVPacket* value){
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        for(;;){
            const int result=decoder.send(value);
            if(result>=0)return;
            require(result==AVERROR(EAGAIN),"nv12_benchmark_send_failed");
            checked_receive();require(Clock::now()<deadline,"nv12_benchmark_submit_timeout");decoder.wait();
        }
    };
    while(av_read_frame(format.value,packet.value)>=0){
        require(++packets<=120,"nv12_benchmark_packet_limit");packet.value->pts=packets;
        submit(packet.value);av_packet_unref(packet.value);checked_receive();
    }
    submit(nullptr);
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    while(checked_receive()!=AVERROR_EOF){
        require(Clock::now()<deadline,"nv12_benchmark_drain_timeout");decoder.wait();
    }
    require(frames>2,"nv12_benchmark_requires_three_frames");
    decoder_owner.reset();dmabuf.after_decoder_close();
    std::cout<<"{\"status\":\"nv12_copy_benchmark_passed\",\"network_used\":false,\"hardware_devices_used\":true"
        <<",\"gpu_sampling_measured\":false,\"width\":"<<width<<",\"height\":"<<height<<",\"decoded_frames\":"<<frames
        <<",\"source_stride\":["<<source_stride[0]<<","<<source_stride[1]<<"],\"destination_stride\":["
        <<destinations[0]->stride[0]<<","<<destinations[0]->stride[1]<<"],\"image_owners\":6"
        <<",\"capture_dmabuf_exported\":"<<(export_result>=0?"true":"false")<<",\"capture_dmabuf_export_errno\":"
        <<(export_result<0?-export_result:0)
        <<",\"bytes_per_copy\":"<<std::uint64_t{width}*height*3/2<<",\"byte_equality_verified\":true,\"methods\":[";
    for(unsigned i=0;i<results.size();++i){
        auto& result=results[i];std::sort(result.ns.begin(),result.ns.end());
        std::uint64_t total=0;for(const auto ns:result.ns)total+=ns;
        if(i)std::cout<<",";
        std::cout<<"{\"name\":\""<<result.name<<"\",\"copies\":"<<result.ns.size()<<",\"total_ns\":"<<total
            <<",\"mean_ns\":"<<total/result.ns.size()<<",\"median_ns\":"<<result.ns[result.ns.size()/2]
            <<",\"p95_ns\":"<<result.ns[(result.ns.size()-1)*95/100]<<"}";
    }
    std::cout<<"]";dmabuf.report();std::cout<<"}\n";return 0;
}
