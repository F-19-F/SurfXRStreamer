#include "host_session.hpp"
#include "../daemon/desktop_module.hpp"
#include "diagnostics.hpp"
#include <thread>

extern "C" __declspec(dllexport) int frame_desktop_session(const surfxr::daemon::DesktopSession* api){
    using namespace surfxr::transport;
    using namespace surfxr::transport::desktop;
    if(!api||api->bytes!=sizeof(*api)||api->version!=surfxr::daemon::desktop_module_version||
       !api->stopped||!api->readable||!api->receive||!api->control||!api->video||!api->monitors||!api->prefer_pc_mouse||
       api->fps<30||api->fps>144||api->bitrate<1||api->bitrate>200||api->monitor>15)return 2;
    std::atomic_bool done{false},stop{false};
    std::thread cancellation([&]{while(!done.load()){if(api->stopped(api->owner))stop.store(true);std::this_thread::sleep_for(std::chrono::milliseconds(2));}});
    struct Join {std::atomic_bool& done;std::thread& thread;~Join(){done.store(true);thread.join();}} join{done,cancellation};
    try{
        Options options;options.embedded=true;options.allow_input=api->input;options.fps=api->fps;
        options.bitrate=api->bitrate;options.monitor=api->monitor;
        options.monitors=[&](unsigned count){return api->monitors(api->owner,count);};
        options.prefer_pc_mouse=[&]{return api->prefer_pc_mouse(api->owner);};
        // Reuse production pacing and cancellation; the daemon encrypts shards.
        Key key{};
        options.send_video=[&](const Bytes& bytes){require(api->video(api->owner,bytes.data(),bytes.size()),"desktop_module_video_failed");};
        Control control{Socket{}};
        control.socket.poll=[&](unsigned milliseconds){return api->readable(api->owner,milliseconds);};
        control.write=[&](const Bytes& bytes){require(api->control(api->owner,bytes.data(),bytes.size()),"desktop_module_control_failed");};
        control.read=[&]{Bytes bytes(2*1024*1024);const auto size=api->receive(api->owner,bytes.data(),bytes.size());require(size>0&&static_cast<std::size_t>(size)<=bytes.size(),"desktop_module_control_closed");bytes.resize(static_cast<std::size_t>(size));return bytes;};
        run_media_session(options,control,Socket{},sockaddr_in{},key,key,api->multimonitor,std::nullopt,stop);
        return 0;
    }catch(const std::exception& error){if(!stop.load())report_failure(error,"desktop_module_failed");return stop.load()?0:1;}
    catch(...){if(!stop.load())report_event("desktop_module_failed");return stop.load()?0:1;}
}
