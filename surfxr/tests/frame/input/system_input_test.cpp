#include "surfxr/system_input.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace surfxr::frame_input;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void policy_checks(){
    CapturePolicy missing;missing.advance(1,3,1);missing.request(true,1);
    check(!missing.snapshot(1).capture,"full capture needs observed click and touch on both hands");
    CapturePolicy policy;std::uint64_t t=1'000'000'000;
    policy.advance(t,3,3);
    check(policy.update(0,false,true,t)&&policy.forwarded(0,false),"system passes before capture");
    policy.update(0,false,false,t);policy.request(true,t);
    check(policy.snapshot(t).capture,"manual entry");
    check(!policy.update(1,true,true,t)&&policy.snapshot(t).touches==2,"touch sampled and suppressed");
    for(unsigned tap=0;tap<4;++tap){
        t+=150'000'000;policy.request(true,t);
        check(!policy.update(0,false,true,t),"system down suppressed");
        check(!policy.update(0,false,false,t+60'000'000),"escape release suppressed");
        check(bool(policy.snapshot(t).capture)==(tap<3),"four completed clicks exit capture");
    }
    check(policy.snapshot(t).exit_serial==1&&policy.forwarded(1,true),"exit restores touch and reports transition");
    policy.request(true,t);check(!policy.snapshot(t).capture,"heartbeat cannot re-enter after escape");
    check(policy.update(0,false,true,t),"next system click reaches headset");policy.update(0,false,false,t);
    policy.request(false,t);policy.request(true,t);check(policy.snapshot(t).capture,"explicit new entry works");
    policy.update(0,false,true,t);
    policy.advance(t+lease_ns+1,3,3);
    check(!policy.snapshot(t).capture&&!policy.forwarded(0,false),"lease restores route without replaying held down");
    check(!policy.update(0,false,false,t+lease_ns+2),"held release is neutral");
    check(policy.update(0,false,true,t+lease_ns+3),"following press passes through");
    policy.stop();policy.update(0,false,false,t+lease_ns+4);
    t+=1'000'000'000;policy.request(true,t);
    check(policy.snapshot(t).capture,"manual capture after disconnect cleanup");
    policy.advance(t,1,1);check(!policy.snapshot(t).capture,"controller loss restores route");

    CapturePolicy replaced;replaced.advance(t,3,3);replaced.request(true,t);
    replaced.update(1,false,true,t);replaced.advance(t,0,0);
    check(!replaced.update(1,false,true,t),"device reassociation cannot replay captured held click");
    replaced.advance(t,3,3);replaced.request(true,t);
    check(!replaced.snapshot(t).capture,"device replacement does not auto re-enter capture");
    replaced.update(1,false,false,t);
    check(replaced.update(1,false,true,t),"next physical press after reassociation reaches headset");

    CapturePolicy separate;separate.advance(t,3,3);separate.request(true,t);
    for(unsigned hand:{0u,0u,0u,1u}){
        t+=150'000'000;separate.request(true,t);separate.update(hand,false,true,t);separate.update(hand,false,false,t+60'000'000);
    }
    check(separate.snapshot(t).capture,"presses from different controllers do not combine");
    t+=2'100'000'000;separate.request(true,t);separate.update(0,false,true,t);separate.update(0,false,false,t+60'000'000);
    check(separate.snapshot(t).capture,"old click burst expires");
    t+=150'000'000;separate.request(true,t);separate.update(0,false,true,t);separate.update(0,false,false,t+5'000'000);
    check(separate.snapshot(t).capture,"contact bounce is not a completed click");
}
void ipc_checks(){
    char root[]="/tmp/surfxr-system-input-XXXXXX";check(mkdtemp(root),"private fixture root");
    const char* previous=std::getenv("XDG_RUNTIME_DIR");const std::string saved=previous?previous:"";
    setenv("XDG_RUNTIME_DIR",root,1);
    struct Cleanup {std::string root,saved;bool had;~Cleanup(){
        unlink(socket_path().c_str());rmdir((root+"/surfxr-client").c_str());rmdir(root.c_str());
        if(had)setenv("XDG_RUNTIME_DIR",saved.c_str(),1);else unsetenv("XDG_RUNTIME_DIR");
    }} cleanup{root,saved,previous!=nullptr};
    check(private_directory()==0,"verified private runtime");
    const int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);check(listener>=0,"fixture socket");
    sockaddr_un address{};address.sun_family=AF_UNIX;const auto path=socket_path();std::strcpy(address.sun_path,path.c_str());
    check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener,1)==0,"isolated local listener");
    SystemInputClient client;client.poll(true,monotonic_ns());
    int peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0&&same_user(peer),"same-user peer");
    const auto request=[&](bool expected){Request value;check(recv(peer,&value,sizeof(value),0)==sizeof(value),"request boundary");
        check(value.tag==magic&&value.revision==version&&value.sampled_ns&&bool(value.capture)==expected,"production client request");};
    const auto reply=[&](bool active,std::uint64_t serial,unsigned available=3,unsigned clicks=0){Snapshot value;value.sampled_ns=monotonic_ns();value.available=value.touch_available=available;value.clicks=clicks;
        value.skeletons[1].sampled_ns=value.sampled_ns;value.skeletons[1].bones[30].position[0]=.031f;
        value.capture=active;value.exit_serial=serial;check(send(peer,&value,sizeof(value),MSG_NOSIGNAL)==sizeof(value),"reply");};
    request(false);reply(false,0);const auto ready=client.poll(true,monotonic_ns());
    check(ready.available==3&&!ready.capture&&ready.skeletons[1].sampled_ns&&ready.skeletons[1].bones[30].position[0]==.031f,"native bones arrive independently of capture");request(false);
    client.request(true);reply(false,0);client.poll(true,monotonic_ns());request(true);
    reply(true,0);check(client.poll(true,monotonic_ns()).capture,"capture acknowledgement");request(true);
    reply(false,1);check(!client.poll(true,monotonic_ns()).capture&&!client.requested(),"driver escape clears viewer request");request(false);
    client.request(true);reply(true,1);client.poll(false,monotonic_ns());request(false);
    check(!client.requested(),"focus loss clears pending entry");
    Snapshot malformed;malformed.sampled_ns=monotonic_ns();malformed.available=3;
    malformed.skeletons[1].sampled_ns=malformed.sampled_ns;malformed.skeletons[1].bones[30].orientation[0]=std::numeric_limits<float>::quiet_NaN();
    send(peer,&malformed,sizeof(malformed),MSG_NOSIGNAL);
    check(client.poll(true,monotonic_ns()).available==0,"invalid snapshot fails closed");
    close(peer);
    SystemInputClient automatic;automatic.session(true);automatic.suspend();automatic.poll(false,monotonic_ns());
    peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0,"automatic stream peer");request(false);
    reply(false,1,2);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"automatic entry waits for both physical controllers");
    reply(false,1,3,1);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"automatic entry waits for released system clicks");
    reply(false,1);automatic.poll(false,monotonic_ns());request(false);
    check(!automatic.requested(),"automatic entry waits for valid focused tracking");
    check(!automatic.poll(true,monotonic_ns()).capture&&automatic.requested(),"automatic request waits for acknowledgement");request(true);
    reply(true,1);check(automatic.poll(true,monotonic_ns()).capture,"new VR stream automatically captures");request(true);
    reply(false,2);automatic.poll(true,monotonic_ns());request(false);
    automatic.session(true);reply(false,2);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"four-click exit cannot auto re-enter the same stream");
    automatic.request(true);automatic.poll(true,monotonic_ns());request(true);
    reply(true,2);check(automatic.poll(true,monotonic_ns()).capture,"manual re-entry after escape remains available");request(true);
    automatic.request(false);automatic.poll(true,monotonic_ns());request(false);
    reply(false,3);automatic.session(true);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"manual off remains off in the same stream");
    automatic.session(false);request(false);close(peer);
    automatic.poll(false,monotonic_ns());
    peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0,"desktop observer peer");request(false);
    reply(false,3,3,1);const auto desktop=automatic.poll(false,monotonic_ns());request(false);
    check(desktop.available==3&&desktop.clicks==1&&!desktop.capture&&!automatic.requested(),"desktop observes System without capturing it");
    reply(false,3);check(!automatic.poll(false,monotonic_ns()).clicks,"desktop observes System release");request(false);
    automatic.session(true);request(false);close(peer);
    automatic.session(true);automatic.poll(true,monotonic_ns());
    peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0,"next VR stream peer");request(false);
    reply(false,3);automatic.poll(true,monotonic_ns());request(true);
    reply(true,3);check(automatic.poll(true,monotonic_ns()).capture,"desktop to VR rearms default entry");request(true);
    reply(true,3);automatic.poll(false,monotonic_ns());request(false);
    reply(false,4);automatic.session(true);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"focus regain does not override release after capture");
    automatic.suspend();request(false);close(peer);
    automatic.session(true);automatic.poll(true,monotonic_ns()+2'000'000'000);
    peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0,"same-stream IPC recovery peer");request(false);
    reply(false,4);automatic.poll(true,monotonic_ns());request(false);
    check(!automatic.requested(),"IPC reconnect after suspension cannot rearm the same stream");
    automatic.close();close(peer);unlink(path.c_str());
    SystemInputClient delayed;delayed.session(true);delayed.poll(true,monotonic_ns());
    const int late_listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);check(late_listener>=0,"late driver listener");
    check(bind(late_listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(late_listener,1)==0,"late driver startup");
    delayed.poll(true,monotonic_ns()+2'000'000'000);
    peer=accept4(late_listener,nullptr,nullptr,SOCK_CLOEXEC);check(peer>=0,"late driver peer");request(false);
    reply(false,0);delayed.poll(true,monotonic_ns());request(true);
    reply(true,0);check(delayed.poll(true,monotonic_ns()).capture,"initial driver unavailability does not discard automatic entry");request(true);
    delayed.close();close(peer);close(late_listener);close(listener);
}
}
#ifdef SURFXR_INPUT_HOOK_TEST
void hook_checks();
#endif
int main(){try{
    policy_checks();ipc_checks();
#ifdef SURFXR_INPUT_HOOK_TEST
    hook_checks();
#endif
    std::cout<<"system input policy, local IPC and hooks passed; no SteamVR or hardware input\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
