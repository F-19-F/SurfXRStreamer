#include "surfxr/surfxr_settings.hpp"
#include "surfxr/settings_store.hpp"
namespace surfxr::transport {
namespace {
Bytes snapshot(std::uint64_t ack,std::uint64_t revision,double fps,double quality,double bitrate){
    Writer w;w.integer(ack,8);w.integer(revision,8);
    for(double value:{fps,quality,bitrate}){std::uint64_t bits;std::memcpy(&bits,&value,8);w.integer(bits,8);}return w.bytes;
}
}
void settings_sync_checks(){
    RemoteSettings settings;auto caps=settings.snapshot().capabilities;
    caps.connected=true;caps.surfxr_video_controls=true;caps.server_video_controls=false;caps.vr_framerates={60,72,80,90,120,144};
    settings.set_capabilities(caps);SettingsSync sync(settings);
    require(!sync.request({SettingId::VrFramerate,90}),"settings_need_initial_snapshot");
    sync.receive(snapshot(0,1,72,4,30));
    require(settings.snapshot().at(SettingId::VrFramerate).requested==72,"settings_pc_initial_authority");
    require(sync.request({SettingId::VrFramerate,144}),"settings_surfxr_144_supported");
    require(sync.request({SettingId::VrBitrateMbps,800}),"settings_surfxr_bitrate_range");
    sync.receive(snapshot(0,2,90,3,40)); // PC edit races an unacknowledged UI edit.
    auto state=settings.snapshot();
    require(state.at(SettingId::VrFramerate).requested==144&&state.at(SettingId::VrGraphicsQuality).requested==3&&state.at(SettingId::VrBitrateMbps).requested==800,"settings_merge_pending_fields");
    sync.receive(snapshot(1,3,144,3,40));
    sync.receive(snapshot(2,4,144,3,800));
    sync.receive(snapshot(0,3,72,4,30)); // Delayed snapshot cannot undo a newer commit.
    require(settings.snapshot().at(SettingId::VrBitrateMbps).requested==800,"settings_reject_old_revision");
    SettingsPreferences preferences;
    require(requested_settings_preferences(settings.snapshot(),preferences),"settings_surfxr_preserves_local_store");
    require(preferences.surfxr_video&&preferences.values[0]==144&&preferences.values[2]==800,"settings_surfxr_snapshot_saved");
    SettingsPreferences restored;require(parse_settings_preferences(encode_settings_preferences(preferences),restored)&&restored.surfxr_video&&restored.values==preferences.values,"settings_surfxr_saved_roundtrip");
    auto extended=snapshot(0,5,144,3,800);Writer policy;policy.integer(0,8);
    extended.insert(extended.end(),policy.bytes.begin(),policy.bytes.end());sync.receive(extended);
    require(sync.request({SettingId::DesktopPointerPriority,1}),"pointer_edit_queued");
    auto edit=sync.poll();require(edit[16]==0x85,"pointer_action_versioned");
    sync.receive(extended);
    require(requested_settings_preferences(settings.snapshot(),preferences)&&preferences.at(SettingId::DesktopPointerPriority)==0,"pointer_pending_not_saved_as_accepted");
    require(settings.snapshot().at(SettingId::DesktopPointerPriority).pending&&settings.snapshot().at(SettingId::DesktopPointerPriority).requested==1,"pointer_pending_survives_poll");
    extended=snapshot(3,6,144,3,800);double one=1;std::uint64_t bits;std::memcpy(&bits,&one,8);policy.bytes.clear();policy.integer(bits,8);
    extended.insert(extended.end(),policy.bytes.begin(),policy.bytes.end());sync.receive(extended);
    require(!settings.snapshot().at(SettingId::DesktopPointerPriority).pending&&settings.snapshot().at(SettingId::DesktopPointerPriority).actual==1,"pointer_pc_ack");
    require(requested_settings_preferences(settings.snapshot(),preferences)&&preferences.at(SettingId::DesktopPointerPriority)==1,"pointer_accepted_cache");
    require(parse_settings_preferences(encode_settings_preferences(preferences),restored)&&restored.values==preferences.values,"pointer_cache_roundtrip");
    auto local=settings.snapshot();local.values[static_cast<std::size_t>(SettingId::FoveatedStreaming)]={1,0,true,true};
    require(requested_settings_preferences(local,preferences)&&preferences.at(SettingId::FoveatedStreaming)==1,"pointer_sync_keeps_local_gaze_preference");
    require(sync.request({SettingId::DesktopPointerPriority,0}),"pointer_pending_before_disconnect");
    require(sync.exit(),"settings_exit_queued");
    sync.disconnected();
    require(settings.snapshot().at(SettingId::DesktopPointerPriority).requested==1&&!settings.snapshot().at(SettingId::DesktopPointerPriority).pending,"pointer_disconnect_retains_last_accepted");
    require(!sync.exit(),"settings_disconnected_exit_disabled");
    const auto poll=sync.poll();Reader read(poll);read.integer(8);
    require(read.integer(8)==0&&read.integer(1)==0x80,"settings_reconnect_never_replays_saved_edits");
    sync.receive(snapshot(0,1,90,-2,0));
    require(settings.snapshot().at(SettingId::VrGraphicsQuality).requested==-2,"settings_custom_resolution_preserved");
}
}
