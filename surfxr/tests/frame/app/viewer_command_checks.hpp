#pragma once
#include "surfxr/viewer_commands.hpp"

namespace surfxr::tests {
inline void viewer_command_checks(const char* directory){
    const auto require=[](bool value,const char* name){if(!value)throw std::runtime_error(name);};
    surfxr::ViewerInstanceLock owned(directory);surfxr::ViewerModeCommands commands(directory);
    const auto socket_path=std::string(directory)+"/viewer-command.sock";
    struct stat info{};require(stat(socket_path.c_str(),&info)==0&&(info.st_mode&0777)==0600,"mode_socket_not_private");
    const int sender=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
    require(sender>=0,"mode_sender_failed");
    sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,socket_path.c_str());
    const auto send=[&](const std::string& data){require(sendto(sender,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==static_cast<ssize_t>(data.size()),"mode_send_failed");};
    require(!commands.poll(),"mode_poll_must_not_invent_command");
    send("monitors 1\n");send("monitors 3\n");commands.poll();
    require(commands.poll_monitors()==std::optional<unsigned>{3}&&!commands.poll_monitors(),"monitor_count_not_coalesced");
    for(const auto* invalid:{"monitors 0\n","monitors 5\n","monitors 03\n","monitors -1\n","monitors 3\nextra"})send(invalid);
    commands.poll();require(!commands.poll_monitors(),"invalid_monitor_count_accepted");
    send("vr\n");require(commands.poll()==std::optional<bool>{true},"mode_vr_not_received");
    send("vr\n");send("desktop\n");require(commands.poll()==std::optional<bool>{false},"mode_latest_command_not_preserved");
    send("VR\n");send("vr\nextra");send(std::string(40,'x'));require(!commands.poll(),"mode_invalid_or_truncated_request_accepted");
    send("desktop\n");send("vr\n");require(commands.poll()==std::optional<bool>{true},"mode_reverse_order_not_preserved");
    send("set 2 300\n");send("set 2 500\n");send("set 1 -1\n");send("set 7 0\n");send("set 7 1\n");commands.poll();
    auto setting=commands.poll_setting();require(setting&&setting->first==1&&setting->second==-1,"setting_quality_not_received");
    setting=commands.poll_setting();require(setting&&setting->first==2&&setting->second==500,"setting_not_coalesced");setting=commands.poll_setting();require(setting&&setting->first==7&&setting->second==1&&!commands.poll_setting(),"gaze_setting_not_coalesced");
    send("set 0 145\n");send("set 2 1001\n");send("set 3 1\n");send("set 7 2\n");send("set 2 200x\n");send("set 1 -2\n");commands.poll();
    require(!commands.poll_setting(),"invalid_setting_command_accepted");
    send("set 9 1\n");commands.poll();setting=commands.poll_setting();require(setting&&setting->first==9&&setting->second==1,"pointer_policy_command_missing");
    send("set 9 2\n");send("set 8 1\n");commands.poll();require(!commands.poll_setting(),"reserved_or_invalid_pointer_command_accepted");
    send("quality 0 0\n");send("quality 1 1\n");send("quality 2 10\n");send("quality 2 75\n");send("quality 3 0\n");send("quality 4 1\n");send("quality 4 0\n");commands.poll();
    for(unsigned id=0;id<5;++id){const auto q=commands.poll_quality();require(q&&q->first==id&&q->second==(id==1?1:id==2?75:0),"quality_command_not_coalesced");}
    require(!commands.poll_quality()&&!commands.poll_setting(),"quality_command_leaked_into_pc_settings");
    for(const auto* invalid:{"quality 5 1\n","quality 4 2\n","quality 0 2\n","quality 2 101\n","quality 1 -1\n","quality 2 1x\n","quality 3 0\nextra"})send(invalid);
    commands.poll();require(!commands.poll_quality(),"invalid_quality_command_accepted");
    send("input 0 0\n");send("input 0 1\n");commands.poll();
    const auto input_pref=commands.poll_input();
    require(input_pref&&input_pref->first==0&&input_pref->second==1,"controller_preference_not_coalesced");
    send("input 1 1\n");send("input 2 1\n");commands.poll();const auto hud=commands.poll_input(),gaze=commands.poll_input();
    require(hud&&hud->first==1&&hud->second==1&&gaze&&gaze->first==2&&gaze->second==1,"overlay_commands_not_delivered");
    send("input 1 1\n");send("input 0 0\n");send("input 2 1\n");send("input 1 0\n");send("input 2 0\n");commands.poll();
    const auto controller_off=commands.poll_input(),hud_off=commands.poll_input(),gaze_off=commands.poll_input();
    require(controller_off&&controller_off->first==0&&controller_off->second==0&&hud_off&&hud_off->first==1&&hud_off->second==0&&gaze_off&&gaze_off->first==2&&gaze_off->second==0,"input_preference_commands_overwrote_each_other");
    require(!commands.poll_input()&&!commands.poll_quality()&&!commands.poll_setting(),"controller_preference_leaked_to_other_queue");
    send("input 3 0\n");commands.poll();const auto boundary=commands.poll_input();
    require(boundary&&boundary->first==3&&boundary->second==0,"boundary_preference_uses_existing_queue");
    send("input 4 1\n");commands.poll();const auto raw_battery=commands.poll_input();
    require(raw_battery&&raw_battery->first==4&&raw_battery->second==1,"raw_battery_preference_uses_existing_queue");
    for(unsigned id=5;id<kInputPreferenceCount;++id){
        const auto& spec=kInputPreferenceSpecs[id];
        send("input "+std::to_string(id)+" "+std::to_string(spec.minimum)+"\n");commands.poll();
        const auto low=commands.poll_input();require(low&&low->first==id&&low->second==spec.minimum,"mini_hud_minimum_command_missing");
        send("input "+std::to_string(id)+" "+std::to_string(spec.maximum)+"\n");commands.poll();
        const auto high=commands.poll_input();require(high&&high->first==id&&high->second==spec.maximum,"mini_hud_maximum_command_missing");
        send("input "+std::to_string(id)+" "+std::to_string(spec.minimum-1)+"\n");
        send("input "+std::to_string(id)+" "+std::to_string(spec.maximum+1)+"\n");commands.poll();
        require(!commands.poll_input(),"invalid_mini_hud_range_accepted");
    }
    send("input 12 22\n");send("input 14 135\n");send("input 12 78\n");commands.poll();
    const auto mini_x=commands.poll_input(),mini_scale=commands.poll_input();
    require(mini_x&&mini_x->first==12&&mini_x->second==78&&mini_scale&&mini_scale->first==14&&mini_scale->second==135&&!commands.poll_input(),"mini_hud_numeric_values_not_coalesced_independently");
    for(const auto* invalid:{"input 16 0\n","input 4 2\n","input 0 2\n","input 0 -1\n","input 0 1x\n","input 0 0\nextra", "input 00 1\n","input 0 -0\n","input 12 023\n"})send(invalid);
    commands.poll();require(!commands.poll_input(),"invalid_controller_preference_accepted");
    require(!commands.poll_timing(),"timing_command_invented");
    send("timing 1\n");commands.poll();require(commands.poll_timing()==std::optional<bool>{true}&&!commands.poll_timing(),"timing_enable_not_consumed");
    send("timing 1\n");send("timing 0\n");commands.poll();
    require(commands.poll_timing()==std::optional<bool>{false},"timing_latest_command_not_preserved");
    for(const auto* invalid:{"timing 2\n","timing -1\n","timing 01\n","timing 1","timing 1\nextra","timing  1\n"})send(invalid);
    commands.poll();require(!commands.poll_timing()&&!commands.poll_setting()&&!commands.poll_quality(),"invalid_timing_command_accepted");
    send("nv12-fuse 0\n");send("nv12-fuse 1\n");commands.poll();
    require(commands.poll_nv12_fused()==std::optional<bool>{true}&&!commands.poll_nv12_fused(),"NV12_diagnostic_not_coalesced");
    send("nv12-fuse 0\n");commands.poll();require(commands.poll_nv12_fused()==std::optional<bool>{false},"NV12_diagnostic_disable_missing");
    for(const auto* invalid:{"nv12-fuse 2\n","nv12-fuse -1\n","nv12-fuse 01\n","nv12-fuse 1\nextra"})send(invalid);
    commands.poll();require(!commands.poll_nv12_fused(),"invalid_NV12_diagnostic_accepted");
    close(sender);
}
}
