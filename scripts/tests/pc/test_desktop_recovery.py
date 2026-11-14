"""Run the production video worker with synthetic capture/encoder owners.

No desktop capture, display changes, input, service, event log or network.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[3] / "surfxr"
sys.path.insert(0, str(PROJECT.parent / 'scripts/transport'))
from source_fixture import production_source, include_directories

CPP = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <objbase.h>
#include "surfxr/surfxr_desktop_layout.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <algorithm>
namespace surfxr::transport::desktop {
using namespace surfxr::transport;
using Clock=std::chrono::steady_clock;
using Key=std::array<std::uint8_t,32>;
unsigned topology=0,discards=0,live_encoders=0;
bool limit=false;
std::vector<unsigned> batches;
struct Options {unsigned monitor=0,fps=144,bitrate=40;bool embedded=false;std::function<void(const Bytes&)> send_video;};
struct Socket {SOCKET value=INVALID_SOCKET;};
struct Control {void send(const Bytes& bytes){validate(layout(bytes));}};
std::vector<unsigned> capture_monitors(unsigned){return topology==1?std::vector<unsigned>{0}:std::vector<unsigned>{0,1,2};}
class Capture {
    unsigned topology_=topology;
    Display display_{};
public:
    unsigned monitor;
    explicit Capture(unsigned id):monitor(id){
        require(id<capture_monitors(0).size(),"fake_missing_display");
        display_={static_cast<int>(id)*128,0,topology==1?256:128,128,topology==1?256u:128u,128};
    }
    const Display& display()const{return display_;}
    bool poll(){require(topology_==topology,"fake_access_lost");return true;}
    void compose_cursor(){}
};
struct Encoded {Bytes bytes;};
struct Encoder {
    Encoder(Capture& c,unsigned,unsigned){require(!limit||!c.monitor,"fake_secondary_encoder_limit");++live_encoders;}
    ~Encoder(){--live_encoders;}
    Bytes configuration(){return {1};}
    Encoded encode(Capture&,bool,std::uint64_t){return {{1}};}
};
VIDEO_STATE
struct VideoBatch {unsigned generation=1,count=1;std::array<std::optional<Encoded>,4> pictures;};
class VideoSender {
    State& state_;
public:
    VideoSender(const Key&,unsigned,State& s,bool,std::function<void(const Bytes&)>,bool=true):state_(s){}
    void discard(){++discards;}
    bool submit(VideoBatch b){
        batches.push_back(b.count);
        require(state_.requested_count.load()==3,"saved_preference_overwritten");
        if(limit){require(b.count==1,"secondary_failure_lost_primary");state_.stop.store(true);}
        else if(topology==0){require(b.count==3,"saved_count_not_restored");topology=1;}
        else if(topology==1){
            require(b.count==1&&state_.layout.streams[0].display.width==256,"disconnected_topology_not_restored");topology=2;
        }else {require(b.count==3,"reconnected_displays_not_restored");state_.stop.store(true);}
        return true;
    }
};
struct VideoMetrics {explicit VideoMetrics(const char*){}void reset(unsigned){}void time(unsigned,Clock::time_point){}void frame(unsigned){}void batch(){}};
struct VideoDeadline {void wait(Clock::time_point){}};
void report_event(const char*){}
void report_duration(const char*,Clock::time_point){}
}
VIDEO_SOURCE
int main(){try{
    using namespace surfxr::transport::desktop;
    Options options;Control control;State state;state.requested_count.store(3);state.stream_ready.store(true);
    video_worker(options,control,Socket{},sockaddr_in{},Key{},state,true);
    if(state.error)std::rethrow_exception(state.error);
    require(batches==std::vector<unsigned>({3,1,3})&&discards==2&&!live_encoders,"topology_recovery_failed");
    topology=0;limit=true;batches.clear();State limited;limited.requested_count.store(3);limited.stream_ready.store(true);
    video_worker(options,control,Socket{},sockaddr_in{},Key{},limited,true);
    if(limited.error)std::rethrow_exception(limited.error);
    require(batches==std::vector<unsigned>({1})&&!live_encoders,"encoder_limit_recovery_failed");
    std::cout<<"{\"passed\":true,\"production_worker\":true,\"topology_counts\":[3,1,3],\"saved_count\":3,\"secondary_encoder_failure_preserves_primary\":true}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''


def main():
    owner = PROJECT / 'pc/desktop'
    state = '\n'.join(line for line in production_source(owner / 'video.hpp').splitlines()
                      if not line.startswith('#'))
    source = '\n'.join(line for line in production_source(owner / 'video.cpp').splitlines()
                       if not line.startswith('#include'))
    # State declarations retain their owning namespace, so close our fake owners
    # temporarily rather than rewriting any production methods.
    cpp = CPP.replace('VIDEO_STATE', '}\n' + state + '\nnamespace surfxr::transport::desktop {')
    cpp = cpp.replace('VIDEO_SOURCE', source)
    with tempfile.TemporaryDirectory(prefix='surfxr-desktop-recovery-') as directory:
        root = Path(directory)
        (root / 'check.cpp').write_text(cpp, encoding='utf-8')
        (root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(desktop_recovery LANGUAGES CXX)\n'
            'add_executable(desktop-recovery-check check.cpp)\n'
            'target_compile_features(desktop-recovery-check PRIVATE cxx_std_17)\n'
            'target_link_libraries(desktop-recovery-check PRIVATE ole32 ws2_32)\n'
            f'target_include_directories(desktop-recovery-check PRIVATE {include_directories(PROJECT)})\n')
        for args in (['cmake', '-S', str(root), '-B', str(root / 'build'), '-A', 'x64'],
                     ['cmake', '--build', str(root / 'build'), '--config', 'Release']):
            proc = subprocess.run(args, capture_output=True, text=True, errors='replace')
            if proc.returncode:
                raise RuntimeError(proc.stdout + proc.stderr)
        proc = subprocess.run([str(root / 'build/Release/desktop-recovery-check.exe')],
                              capture_output=True, text=True, timeout=10, check=True)
        print(json.dumps(json.loads(proc.stdout)))


if __name__ == '__main__':
    main()
