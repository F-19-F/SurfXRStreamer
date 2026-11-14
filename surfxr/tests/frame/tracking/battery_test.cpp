#include "surfxr/battery.hpp"
#include "../../../frame/xr/headset_battery_status.hpp"
#if __has_include(<openxr/openxr.h>)
#include "surfxr/openxr_battery.hpp"
#define SURFXR_BATTERY_TEST_HAS_OPENXR 1
#endif
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
namespace fs=std::filesystem;
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
struct Temporary {
    fs::path base=fs::weakly_canonical(fs::temp_directory_path()),path;
    Temporary(){for(unsigned attempt=0;attempt<32;++attempt){const auto candidate=base/("surfxr-battery-test-"+std::to_string(surfxr::battery_monotonic_ns())+"-"+std::to_string(attempt));
            if(fs::create_directory(candidate)){path=candidate;return;}}
        throw std::runtime_error("temporary_fixture_creation_failed");}
    ~Temporary(){std::error_code error;
        if(!path.empty()&&path.parent_path()==base&&path.filename().string().rfind("surfxr-battery-test-",0)==0)fs::remove_all(path,error);}
    fs::path root(const char* name){const auto result=path/name;fs::create_directory(result);return result;}
};
void put(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file<<text;require(bool(file),"fixture_write_failed");}
void supply(const fs::path& path,const std::string& capacity="73\n",const std::string& status="Discharging\n"){
    put(path/"type","Battery\n");put(path/"present","1\n");put(path/"capacity",capacity);put(path/"status",status);
}
template<class Predicate>void eventually(Predicate predicate,const char* label){const auto until=std::chrono::steady_clock::now()+2s;
    do{if(predicate())return;std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<until);throw std::runtime_error(label);
}
unsigned self_test(){
    unsigned groups=0;
    {
        const auto unknown=surfxr::DeviceBattery{};require(unknown.percent==-1&&!unknown.charging&&!unknown.charging_known,"unknown_is_not_full_or_empty");
        require(surfxr::sanitize_battery({-1,true,true}).percent==-1&&surfxr::sanitize_battery({101,true,true}).percent==-1,"invalid_percent_rejected");
        require(surfxr::sanitize_battery({0,true,true}).percent==0&&surfxr::sanitize_battery({100,false,true}).percent==100,"empty_and_full_are_valid");
        require(!surfxr::sanitize_battery({50,true,false}).charging,"unknown_charging_not_fabricated");
    }++groups;
#ifdef SURFXR_BATTERY_TEST_HAS_OPENXR
    {
        require(static_cast<int>(XR_TYPE_BATTERY_STATE_DISPLAY_EXT)==1000836000&&sizeof(XrBatteryStateDisplayStateFlagsEXT)==8,"openxr_battery_abi_type_and_flags");
        XrBatteryStateDisplayEXT state{XR_TYPE_BATTERY_STATE_DISPLAY_EXT,nullptr,XR_BATTERY_STATE_DISPLAY_STATE_VALID_BIT_EXT,.51f};
        require(surfxr::battery_from_openxr(1,state).percent==51,"typed_openxr_battery_mapping");
        require(surfxr::battery_from_openxr(XR_NULL_PATH,state).percent==-1,"typed_null_profile_rejected");
        state.stateFlags=0;state.batteryLevel=std::numeric_limits<float>::quiet_NaN();
        require(surfxr::battery_from_openxr(1,state).percent==-1,"typed_invalid_does_not_consume_level");
        state.stateFlags=XR_BATTERY_STATE_DISPLAY_STATE_VALID_BIT_EXT|XR_BATTERY_STATE_DISPLAY_STATE_NO_BATTERY_BIT_EXT;
        require(surfxr::battery_from_openxr(1,state).percent==-1,"typed_no_battery_not_reported_full");
    }++groups;
#endif
    {
        for(auto flags:{0ull,2ull,4ull,8ull,9ull,15ull})require(surfxr::battery_from_xr_state(true,flags,.75f).percent==-1,"invalid_or_no_battery_xr_state");
        require(surfxr::battery_from_xr_state(false,1,.75f).percent==-1,"null_interaction_profile_rejects_level");
        for(float level:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
            require(surfxr::battery_from_xr_state(true,1,level).percent==-1,"invalid_xr_fraction");
        const auto normal=surfxr::battery_from_xr_state(true,1,.73f),plugged=surfxr::battery_from_xr_state(true,5,.73f),charging=surfxr::battery_from_xr_state(true,7,.73f);
        require(normal.percent==73&&normal.charging_known&&!normal.charging,"valid_xr_percentage");
        require(plugged.percent==73&&!plugged.charging&&charging.charging,"plugged_in_not_same_as_charging");
        require(surfxr::battery_from_xr_state(true,1,0).percent==0&&surfxr::battery_from_xr_state(true,1,1).percent==100,"xr_zero_and_one_bounds");
        require(surfxr::battery_from_xr_state(true,1,.734f).percent==73&&surfxr::battery_from_xr_state(true,1,.736f).percent==74,"xr_rounding_nearest_percent");
    }++groups;
    {
        surfxr::BatterySnapshot value;value.hmd={73,false,true};value.controllers={surfxr::DeviceBattery{17,false,true},surfxr::DeviceBattery{84,true,true}};
        constexpr std::uint64_t start=40'000'000'000ull;value.hmd_sampled_monotonic_ns=value.controllers_sampled_monotonic_ns=start;
        auto state=surfxr::fresh_battery_snapshot(value,start);require(state.hmd.percent==73&&state.controllers[0].percent==17&&state.controllers[1].percent==84,"physical_hands_not_swapped");
        state=surfxr::fresh_battery_snapshot(value,start+5'000'000'000ull);require(state.controllers[1].percent==84,"controller_ttl_inclusive");
        state=surfxr::fresh_battery_snapshot(value,start+5'000'000'001ull);require(state.hmd.percent==73&&state.controllers[0].percent==-1&&state.controllers[1].percent==-1,"controllers_expire_independently");
        state=surfxr::fresh_battery_snapshot(value,start+30'000'000'001ull);require(state.hmd.percent==-1&&!state.hmd.charging_known,"headset_expires");
        require(surfxr::fresh_battery_snapshot(value,start-1).hmd.percent==-1,"future_timestamp_rejected");
        value.hmd_sampled_monotonic_ns=value.controllers_sampled_monotonic_ns=0;
        require(surfxr::fresh_battery_snapshot(value,start).hmd.percent==-1&&surfxr::fresh_battery_snapshot(value,start).controllers[0].percent==-1,"zero_timestamp_rejected");
    }++groups;
    Temporary temporary;
    {
        using surfxr::xr_detail::HeadsetBatteryStatus;
        HeadsetBatteryStatus packet;packet.flags=HeadsetBatteryStatus::HasLevel;packet.level=.99f;
        require(packet.valid()&&packet.battery().percent==99,"runtime_display_percentage_rounding");
        packet.flags|=HeadsetBatteryStatus::ChargingKnown|HeadsetBatteryStatus::Charging;
        require(packet.battery().charging&&packet.battery().charging_known,"runtime_charging");
        packet.level=0;require(packet.battery().percent==0,"runtime_empty_is_valid");
        packet.level=1;require(packet.battery().percent==100,"runtime_full_is_valid");
        for(float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
            packet.level=bad;require(!packet.valid()&&packet.battery().percent==-1,"bad_runtime_level_rejected");
        }
        packet={};packet.flags=HeadsetBatteryStatus::Charging;require(!packet.valid(),"charging_requires_known");
        packet={};packet.flags=8;require(!packet.valid(),"unknown_runtime_flags_rejected");
        packet={};packet.magic=0;require(!packet.valid(),"wrong_runtime_packet_rejected");
        packet={};packet.reserved=1;require(!packet.valid(),"reserved_runtime_field_rejected");
        packet={};require(packet.valid()&&packet.battery().percent==-1,"missing_runtime_level_is_unknown");
    }++groups;
    {
        auto root=temporary.root("runtime");supply(root/"pack","92\n","Charging\n");
        surfxr::BatteryMonitor monitor(root.string(),30s);
        eventually([&]{return monitor.snapshot().hmd.percent==92;},"runtime_fallback_initial_read");
        const auto now=surfxr::battery_monotonic_ns();
        monitor.publish_headset_runtime({99,true,true},now);
        require(monitor.snapshot(now).hmd.percent==99&&monitor.snapshot(now).hmd.charging,"runtime_99_overrides_raw_92");
        require(monitor.snapshot(now).hmd_sampled_monotonic_ns==now,"runtime_original_timestamp_preserved");
        require(monitor.raw_headset(now).percent==92,"raw_display_independent_of_sync_source");
        monitor.set_sync_raw_battery(true);
        require(monitor.snapshot(now).hmd.percent==92&&monitor.snapshot(now).hmd.charging,"raw_sync_uses_system_capacity");
        monitor.set_sync_raw_battery(false);
        require(monitor.snapshot(now).hmd.percent==99,"runtime_sync_restored_without_restart");
        require(monitor.snapshot(now+5'000'000'000ull).hmd.percent==99,"runtime_ttl_inclusive");
        monitor.publish_headset_runtime({99,true,true},now);
        require(monitor.snapshot(now+5'000'000'001ull).hmd.percent==92,"repeated_bridge_sample_cannot_renew_ttl");
        monitor.publish_headset_runtime({100,false,true},now);
        require(monitor.snapshot(now).hmd.percent==100&&!monitor.snapshot(now).hmd.charging,"runtime_full_and_charge_change");
        monitor.publish_headset_runtime({},now);
        require(monitor.snapshot(now).hmd.percent==92,"runtime_failure_falls_back_immediately");
        monitor.publish_headset_runtime({99,false,false},now+1);
        require(monitor.snapshot(now).hmd.percent==92,"future_runtime_sample_rejected");
        monitor.publish_headset_runtime({99,false,false},0);
        require(monitor.snapshot(now).hmd.percent==92,"zero_runtime_sample_rejected");
        require(monitor.snapshot(now+31'000'000'000ull).hmd.percent==-1,"both_sources_expire");
        monitor.publish_headset_runtime({100,true,true},now+31'000'000'000ull);
        monitor.set_sync_raw_battery(true);
        require(monitor.snapshot(now+31'000'000'000ull).hmd.percent==-1,"raw_sync_never_substitutes_adjusted_runtime_value");
    }++groups;
    {
        auto root=temporary.root("single");supply(root/"max1720x_bat_7-36");
        put(root/"usb"/"type","USB\n");put(root/"usb"/"online","1\n");put(root/"usb"/"capacity","100\n");put(root/"usb"/"status","Charging\n");
        const auto read=[&]{return surfxr::read_headset_battery(root.string());};
        require(read().percent==73&&read().charging_known&&!read().charging,"frame_sysfs_ignores_online_usb");
        put(root/"max1720x_bat_7-36"/"status","Charging\n");require(read().charging,"sysfs_charging");
        for(const char* status:{"Discharging\n","Not charging\n","Full\n"}){put(root/"max1720x_bat_7-36"/"status",status);require(read().charging_known&&!read().charging,"sysfs_noncharging_states");}
        put(root/"max1720x_bat_7-36"/"status","Unknown\n");require(read().percent==73&&!read().charging_known,"unknown_status_keeps_valid_capacity");
        put(root/"max1720x_bat_7-36"/"capacity","0\n");require(read().percent==0,"sysfs_empty_not_unknown");
        put(root/"max1720x_bat_7-36"/"capacity","100\n");require(read().percent==100,"sysfs_full");
        for(const auto& bad:std::array<std::string,6>{{"101\n","-1\n","50%\n","99.5\n","",std::string(80,'7')}}){put(root/"max1720x_bat_7-36"/"capacity",bad);require(read().percent==-1,"malformed_sysfs_capacity");}
        put(root/"max1720x_bat_7-36"/"capacity","73\n");put(root/"max1720x_bat_7-36"/"present","0\n");require(read().percent==-1,"absent_battery_rejected");
        require(surfxr::read_headset_battery((root/"missing").string()).percent==-1,"missing_sysfs_is_unknown");
    }++groups;
    {
        auto root=temporary.root("roles");supply(root/"controller","9\n");put(root/"controller"/"scope","Device\n");
        require(surfxr::read_headset_battery(root.string()).percent==-1,"controller_battery_never_used_as_headset");
        supply(root/"system","61\n");put(root/"system"/"scope","System\n");supply(root/"other","99\n");
        require(surfxr::read_headset_battery(root.string()).percent==61,"explicit_system_scope_preferred");
        supply(root/"system2","44\n");put(root/"system2"/"scope","System\n");
        require(surfxr::read_headset_battery(root.string()).percent==-1,"multiple_system_batteries_are_ambiguous");
        auto ambiguous=temporary.root("ambiguous");supply(ambiguous/"first","31\n");supply(ambiguous/"second","91\n");
        require(surfxr::read_headset_battery(ambiguous.string()).percent==-1,"unscoped_batteries_not_chosen_by_directory_order");
        auto bounded=temporary.root("bounded");supply(bounded/"system");for(unsigned i=0;i<64;++i)fs::create_directory(bounded/std::to_string(i));
        require(surfxr::read_headset_battery(bounded.string()).percent==-1,"sysfs_scan_is_bounded");
    }++groups;
    {
        auto root=temporary.root("async");supply(root/"pack","75\n");surfxr::BatteryMonitor monitor(root.string(),30s);
        eventually([&]{return monitor.snapshot().hmd.percent==75;},"async_initial_read");
        put(root/"pack"/"capacity","12\n");
        // Snapshot and controller publication do no sysfs I/O or wait for the
        // sleeping poller. The old reading must remain until the next poll.
        for(unsigned i=0;i<100;++i){monitor.publish_controllers({surfxr::DeviceBattery{17,false,true},surfxr::DeviceBattery{84,true,true}});
            const auto state=monitor.snapshot();require(state.hmd.percent==75&&state.controllers[0].percent==17&&state.controllers[1].percent==84,"snapshot_is_cached_and_roles_stable");}
        monitor.publish_controllers({surfxr::DeviceBattery{},surfxr::DeviceBattery{84,false,true}});const auto state=monitor.snapshot();
        require(state.controllers[0].percent==-1&&state.controllers[1].percent==84,"invalid_hand_clears_prior_value_without_clearing_peer");
        const auto start=std::chrono::steady_clock::now();monitor.stop();monitor.stop();
        require(std::chrono::steady_clock::now()-start<1s,"stop_wakes_poll_wait_instead_of_waiting_thirty_seconds");
    }++groups;
    {
        auto root=temporary.root("failures");supply(root/"pack","58\n");surfxr::BatteryMonitor monitor(root.string(),100ms);
        eventually([&]{return monitor.snapshot().hmd.percent==58;},"async_failure_initial_read");
        put(root/"pack"/"capacity","invalid\n");eventually([&]{return monitor.snapshot().hmd.percent==-1;},"read_failure_replaces_stale_value");
        put(root/"pack"/"capacity","59\n");eventually([&]{return monitor.snapshot().hmd.percent==59;},"async_read_recovers");
        monitor.stop();
        bool rejected=false;try{surfxr::BatteryMonitor invalid(root.string(),0ms);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"busy_loop_poll_interval_rejected");
    }++groups;
    return groups;
}
}
int main(int argc,char** argv){try{
    if(argc==2&&std::strcmp(argv[1],"--read-system")==0){const auto value=surfxr::read_headset_battery();
        std::cout<<"{\"percent\":"<<value.percent<<",\"charging\":"<<(value.charging?"true":"false")<<",\"charging_known\":"<<(value.charging_known?"true":"false")<<"}\n";return 0;}
    if(argc==2&&std::strcmp(argv[1],"--self-test")==0){const auto groups=self_test();
        std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"synthetic_files_only\":true,\"openxr_session_started\":false,\"network_used\":false}\n";return 0;}
    std::cout<<"surfxr-battery-test --self-test | --read-system\n";return argc==1||(argc==2&&std::strcmp(argv[1],"--help")==0)?0:1;
}catch(const std::exception& e){std::cerr<<"battery_test: "<<e.what()<<'\n';return 1;}}
