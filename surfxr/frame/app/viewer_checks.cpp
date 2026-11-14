#include "viewer_private.hpp"
#include "surfxr/surfxr_modes.hpp"
#include "surfxr/surfxr_settings.hpp"
#include "surfxr/surfxr.hpp"
namespace surfxr {void vr_surfxr_self_test();}

namespace surfxr::viewer {
int viewer_self_test(){
        try{
            const SessionOptions defaults;
            if(defaults.initial_vr_framerate!=120||defaults.surfxr_view_width!=3072||
               defaults.surfxr_view_height!=3264||!defaults.initial_foveated_streaming)
                throw std::runtime_error("viewer_session_defaults_match_pc");
            surfxr::cpu_affinity_parser_self_test();
            surfxr::video_decoder_self_test();
            surfxr::ui_input_self_test();
            surfxr::vr_surfxr_self_test();
            {
                DesktopFrameDemand demand;demand.set_mask(3);
                const auto first=demand.epoch(0),second=demand.epoch(1);
                demand.set_mask(3);
                if(!first||!second||demand.epoch(0)!=first||demand.epoch(1)!=second)throw std::runtime_error("desktop_demand_changed_visible_epoch");
                demand.set_mask(2);
                if(demand.epoch(0)||demand.epoch(1)!=second)throw std::runtime_error("desktop_demand_paused_other_monitor");
                demand.set_mask(3);
                if(demand.epoch(0)<=first||demand.epoch(1)!=second)throw std::runtime_error("desktop_demand_resumed_old_epoch");
                demand.set_mask(0);
                for(unsigned id=0;id<4;++id)if(demand.epoch(id))throw std::runtime_error("desktop_demand_not_paused");
                DesktopFrameDemand legacy(false);legacy.set_mask(3);
                if(legacy.enabled()||legacy.token(0)||legacy.epoch(0))throw std::runtime_error("desktop_demand_override_gated_legacy_output");
                std::uint64_t clock=1'000'000'000;surfxr::SessionControlQueue queue;surfxr::RemoteSettings settings;
                surfxr::VrClientState state(queue,settings,{120},false,8192,8192,[&]{return clock;});
                state.set_desktop_renderable(false);state.connected(true);state.source_config(0,2);
                clock+=120'000'000'000;state.tick();auto stats=state.statistics();
                if(!stats.desktop_pending||stats.desktop_timeouts||stats.desktop_keyframe_recoveries||stats.desktop_restart_recoveries)
                    throw std::runtime_error("desktop_hidden_recovery_advanced");
                state.set_desktop_renderable(true);clock+=3'000'000'000;state.tick();
                if(state.statistics().desktop_keyframe_recoveries!=1)throw std::runtime_error("desktop_resume_keyframe_deadline");
                state.set_desktop_renderable(false);clock+=120'000'000'000;state.tick();state.set_desktop_renderable(true);
                clock+=3'000'000'000;state.tick();
                if(state.statistics().desktop_restart_recoveries!=1||state.statistics().desktop_timeouts)throw std::runtime_error("desktop_resume_restart_deadline");
                clock+=4'000'000'000;state.tick();
                if(state.statistics().desktop_timeouts!=1)throw std::runtime_error("desktop_resume_lost_timeout");
            }
            {
                surfxr::transport::Modes modes(true);surfxr::RemoteSettings settings;surfxr::SessionControlQueue commands;
                surfxr::VrClientState state(commands,settings,{144},true,8192,8192);
                state.set_desktop_only(true);state.set_mode_request([&](bool desktop){return modes.request(desktop);});state.connected(true);
                if(!state.launch()||!modes.pending()||modes.begin()||modes.pending())throw std::runtime_error("surfxr_inprocess_vr_request");
                if(!state.desktop()||!modes.pending()||!state.launch()||modes.begin())throw std::runtime_error("surfxr_inprocess_latest_mode");
                state.desktop();if(!modes.begin())throw std::runtime_error("surfxr_inprocess_desktop_request");
                surfxr::SessionControlCommand unused;if(commands.pop(unused))throw std::runtime_error("surfxr_mode_emitted_surfxr_control");
            }
            {
                surfxr::RemoteSettings settings;auto caps=settings.snapshot().capabilities;
                caps.surfxr_video_controls=true;settings.set_capabilities(caps);surfxr::transport::SettingsSync sync(settings);
                surfxr::SettingsPreferences saved;saved.surfxr_video=true;saved.values={144,3,800,90,80,1,.6};
                restore_settings(settings,saved);
                const auto initial=prepare_initial_settings(settings);
                if(!initial.surfxr_video||initial.values[0]!=144||initial.values[2]!=800)
                    throw std::runtime_error("surfxr_saved_settings_startup_failed");
                const auto poll=sync.poll();surfxr::transport::Reader reader(poll);reader.integer(8);
                if(reader.integer(8)!=0||reader.integer(1)!=0x80)throw std::runtime_error("surfxr_cache_replayed_as_edit");
            }
            surfxr::RemoteSettings preferences;surfxr::SessionControlQueue preference_control;
            surfxr::VrClientState preference_state(preference_control,preferences,{120},false,5120,5120);
            surfxr::SettingsPreferences saved;saved.values={90,7,200,90,80,1,.6};
            if(restore_settings(preferences,saved)!=2||preferences.snapshot().at(surfxr::SettingId::VrFramerate).requested!=90
               ||preferences.snapshot().at(surfxr::SettingId::VrGraphicsQuality).requested!=6
               ||preferences.snapshot().at(surfxr::SettingId::VrBitrateMbps).requested!=200
               ||preferences.snapshot().at(surfxr::SettingId::MicrophoneEnabled).requested!=0)
                throw std::runtime_error("saved_preferences_capability_filter_failed");
            preferences.observe({surfxr::SettingId::VrGraphicsQuality,0});
            if(preferences.snapshot().at(surfxr::SettingId::VrGraphicsQuality).requested!=6)throw std::runtime_error("saved_preferences_overwritten_by_server");
            std::atomic_bool cancelled{true};bool triggered=false;
            surfxr::SessionCallbacks callbacks;callbacks.connection_started=[&]{triggered=true;};
            surfxr::SessionOptions cancelled_options;cancelled_options.host="127.0.0.1";
            cancelled_options.seconds=1;
            cancelled_options.backend=surfxr::SessionBackend::Surfxr;
            const auto surfxr_cancelled=surfxr::transport::run(cancelled_options,callbacks,&cancelled);
            if(triggered||surfxr_cancelled.connection_started||surfxr_cancelled.exit_code!=2||!surfxr_cancelled.connection_closed)
                throw std::runtime_error("cancelled_surfxr_session_started_work");
            char name[]="surfxr-viewer",host_option[]="--host",host[]="127.0.0.1",seconds_option[]="--seconds",invalid[]="3601",zero[]="0";
            char* args[]={name,host_option,host,seconds_option,invalid};bool rejected=false;
            try{options(5,args);}catch(const std::exception&){rejected=true;}
            if(!rejected || quoted_error("quote\"line\n")!="\"quote\\\"line\\u000a\"")throw std::runtime_error("argument_or_json_invariant_failed");
            args[4]=zero;
            if(options(5,args).session.seconds!=0||options(3,args).session.seconds!=0)throw std::runtime_error("unlimited_viewer_not_default");
            char backend_option[]="--backend",surfxr_value[]="surfxr",unsupported_value[]="unsupported",port_option[]="--surfxr-control-port",port_value[]="12345";
            char* surfxr_args[]={name,host_option,host,backend_option,surfxr_value,port_option,port_value};
            if(options(3,surfxr_args).session.backend!=surfxr::SessionBackend::Surfxr||
               options(7,surfxr_args).session.backend!=surfxr::SessionBackend::Surfxr||!options(7,surfxr_args).start_vr||
               options(7,surfxr_args).session.surfxr_control_port!=12345)throw std::runtime_error("surfxr_backend_option_failed");
            surfxr_args[4]=unsupported_value;rejected=false;try{options(7,surfxr_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("surfxr_options_accepted_by_surfxr_backend");
            char media_option[]="--surfxr-media-encryption",aes_value[]="aes",off_value[]="off",bad_media[]="bad";
            char* media_args[]={name,host_option,host,backend_option,surfxr_value,media_option,aes_value};
            if(options(5,media_args).session.surfxr_media_aes||!options(7,media_args).session.surfxr_media_aes)throw std::runtime_error("surfxr_media_encryption_default_or_option");
            media_args[6]=off_value;if(options(7,media_args).session.surfxr_media_aes)throw std::runtime_error("surfxr_media_encryption_off");
            media_args[6]=bad_media;rejected=false;try{options(7,media_args);}catch(...){rejected=true;}
            if(!rejected)throw std::runtime_error("surfxr_media_encryption_invalid_option");
            char version_option[]="--surfxr-version",master_version[]="21.0.0-dev14",bad_version[]="21.0.0-dev13";
            char* version_args[]={name,host_option,host,backend_option,surfxr_value,version_option,master_version};
            if(options(5,version_args).session.surfxr_version!="21.0.0-dev14"||options(7,version_args).session.surfxr_version!=master_version)
                throw std::runtime_error("surfxr_version_option_failed");
            version_args[6]=bad_version;rejected=false;try{options(7,version_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("unsupported_surfxr_version_accepted");
            char desktop_flag[]="--surfxr-desktop",desktop_key_option[]="--surfxr-desktop-key-file",desktop_key[]="/private/surfxr-desktop.key",v20[]="20.14.1";
            char* desktop_args[]={name,host_option,host,backend_option,surfxr_value,version_option,master_version,desktop_flag,desktop_key_option,desktop_key};
            const auto desktop_options=options(10,desktop_args);
            if(!desktop_options.session.surfxr_desktop||desktop_options.start_vr||desktop_options.session.surfxr_desktop_port!=19950||desktop_options.session.surfxr_desktop_key_file!=desktop_key)throw std::runtime_error("surfxr_desktop_option_failed");
            desktop_args[6]=v20;rejected=false;try{options(10,desktop_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("surfxr_v20_desktop_accepted");
            desktop_args[6]=master_version;desktop_args[4]=unsupported_value;rejected=false;try{options(10,desktop_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("unsupported_desktop_backend_accepted");
            char affinity_option[]="--cpu-affinity",available[]="available",inherit[]="inherit",bad_affinity[]="0-7";
            char* affinity_args[]={name,host_option,host,affinity_option,available,affinity_option,inherit};
            if(options(3,affinity_args).available_cpu_affinity||!options(5,affinity_args).available_cpu_affinity)
                throw std::runtime_error("cpu_affinity_option_default_failed");
            affinity_args[4]=inherit;
            if(options(5,affinity_args).available_cpu_affinity)throw std::runtime_error("cpu_affinity_inherit_option_failed");
            rejected=false;try{options(7,affinity_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("duplicate_cpu_affinity_option_accepted");
            affinity_args[4]=bad_affinity;rejected=false;try{options(5,affinity_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("invalid_cpu_affinity_option_accepted");
            char motion_option[]="--test-head-motion",trajectory[]="yaw-pitch",bad_motion[]="on";
            char* motion_args[]={name,host_option,host,motion_option,trajectory,motion_option,trajectory};
            if(options(3,motion_args).test_head_motion||!options(5,motion_args).test_head_motion)
                throw std::runtime_error("test_head_motion_must_be_explicit");
            rejected=false;try{options(7,motion_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("duplicate_test_head_motion_accepted");
            motion_args[4]=bad_motion;rejected=false;try{options(5,motion_args);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("invalid_test_head_motion_accepted");
            std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;
        }catch(const std::exception& error){std::cerr<<"{\"status\":\"self_test_failed\",\"network_used\":false,\"openxr_session_started\":false,\"error\":"<<quoted_error(error.what())<<"}\n";return 1;}
        catch(...){std::cerr<<"{\"status\":\"self_test_failed\",\"network_used\":false,\"openxr_session_started\":false}\n";return 1;}
}
}
