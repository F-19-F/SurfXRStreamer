#include "receiver_worker.hpp"
#include "viewer_ui.hpp"
#include "surfxr/surfxr_feedback.hpp"
#include "surfxr/surfxr_modes.hpp"
#include "surfxr/surfxr_settings.hpp"
#include "surfxr/xr_boundary.hpp"

namespace surfxr::viewer {
std::atomic_bool stop_requested{false};
static_assert(std::atomic_bool::is_always_lock_free,"Signal handler requires lock-free stop flag");
extern "C" void stop_signal(int){stop_requested.store(true,std::memory_order_relaxed);}
struct SignalHandlers {
    struct sigaction previous_int{},previous_term{};
    SignalHandlers(){
        struct sigaction action{};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);
        if(sigaction(SIGINT,&action,&previous_int)!=0)throw std::runtime_error("signal_setup_failed");
        if(sigaction(SIGTERM,&action,&previous_term)!=0){sigaction(SIGINT,&previous_int,nullptr);throw std::runtime_error("signal_setup_failed");}
    }
    ~SignalHandlers(){sigaction(SIGINT,&previous_int,nullptr);sigaction(SIGTERM,&previous_term,nullptr);}
};

}
using namespace surfxr::viewer;

int main(int argc,char** argv){
    if(argc==2 && std::strcmp(argv[1],"--self-test")==0)return viewer_self_test();

    if(argc==2 && std::strcmp(argv[1],"--help")==0){
        std::cout<<"SurfXR Frame viewer\n"
            "--host PC_IPV4 [--local-address FRAME_IPV4] [--surfxr-desktop]\n"
            "--surfxr-version 21.0.0-dev14|20.14.1 (default 21.0.0-dev14)\n"
            "--seconds 0..3600 (0: until disconnect) --decoder v4l2|software\n"
            "--audio on|off --microphone on|off --input on|off\n"
            "--cpu-affinity available|inherit --status-file /PATH\n"
            "PC trust is required. v21 supports desktop/VR switching in the same viewer.\n";
        return 0;
    }
    ViewerReport result;
    try{
        auto opts=options(argc,argv);
        result.backend=opts.session.backend;
        surfxr::TestHeadMotion test_head_motion(opts.test_head_motion);
        result.instance_lock=std::make_unique<surfxr::ViewerInstanceLock>();
        result.cpu_affinity=surfxr::configure_cpu_affinity(opts.available_cpu_affinity);
        surfxr::ViewerModeCommands mode_commands;
        result.runtime_status=std::make_unique<surfxr::ViewerStatusWriter>(opts.status_path,opts.start_vr);
        surfxr::UiLayout saved_layout;
        auto layout_status=surfxr::LayoutStoreStatus::Missing;
        if(!opts.layout_path.empty()){
            layout_status=surfxr::load_layout(opts.layout_path,saved_layout);
            result.layout_load=surfxr::layout_store_status_name(layout_status);
        }
        for(auto& decoder:result.decoders)decoder.backend=opts.backend;
        result.audio_requested=opts.audio;
        result.microphone_requested=opts.session.microphone_enabled;
        result.input_requested=opts.input;
        auto input=std::make_shared<surfxr::SessionInputQueue>();
        if(opts.session.surfxr_desktop||opts.session.surfxr_modes)input->use_desktop_events();
        auto control=std::make_shared<surfxr::SessionControlQueue>();opts.session.control_queue=control;
        opts.session.haptics_queue=result.haptics;
        if(opts.input)opts.session.input_queue=input;
        SignalHandlers handlers;
        result.stage="xr_initialization";
        surfxr::XrDisplay display(opts.session.backend==surfxr::SessionBackend::Surfxr&&opts.session.surfxr_version=="21.0.0-dev14");
        opts.session.pose_time_available=true;
        // Desktop slots can retain mapped Vulkan frames after the producer
        // stops. Declare them after display so normal and exceptional teardown
        // destroys every published frame before the owning VkDevice.
        std::array<surfxr::LatestFrameSlot,4> slots;
        std::array<std::atomic_bool,4> available{};
        const auto* demand_override=std::getenv("SURFXR_DESKTOP_DEMAND");
        DesktopFrameDemand desktop_demand(!demand_override||std::strcmp(demand_override,"0")!=0);
        for(auto& enabled:available)enabled.store(false);
        std::vector<surfxr::XrPanelSource> panels;
        for(std::uint32_t i=0;i<4;++i)panels.push_back({i,&slots[i],"DISPLAY "+std::to_string(i+1),{},&available[i],desktop_demand.token(i)});
        surfxr::RemoteSettings settings;
        settings.seed_requested({surfxr::SettingId::MicrophoneEnabled,opts.session.microphone_enabled?1.0:0.0});
        const auto& runtime=display.statistics();
        if(opts.session.backend==surfxr::SessionBackend::Surfxr){
            // Streaming cadence is independent of the local panel mode. Frame's
            // runtime may enumerate only 144 Hz; lower PC rates are presented
            // through the existing timestamp-based pacer without changing XR.
            opts.session.surfxr_refresh_rates={60,72,80,90,120,144};
            opts.session.surfxr_feedback=std::make_shared<surfxr::transport::FeedbackQueue>();
        }
        opts.session.eye_gaze_available=runtime.eye_gaze_supported;
        auto gaze_capabilities=settings.snapshot().capabilities;gaze_capabilities.eye_gaze_available=runtime.eye_gaze_supported;
        settings.set_capabilities(std::move(gaze_capabilities));
        surfxr::VrClientState vr(*control,settings,runtime.refresh_rates_hz,opts.session.microphone_enabled,
                             runtime.max_render_width,runtime.max_render_height);
        vr.set_renderable(false);
        if(desktop_demand.enabled())vr.set_desktop_renderable(false);
        vr.set_vr_only(opts.session.backend==surfxr::SessionBackend::Surfxr&&!opts.session.surfxr_desktop);
        vr.set_desktop_only(opts.session.surfxr_desktop);
        if(opts.session.surfxr_modes)vr.set_mode_request([modes=opts.session.surfxr_modes](bool desktop){return modes->request(desktop);});
        surfxr::SettingsStore settings_store(opts.settings_path);
        if(opts.session.surfxr_modes){
            auto sync=std::make_shared<surfxr::transport::SettingsSync>(settings,[&]{settings_store.submit(settings.snapshot());});opts.session.surfxr_settings=sync;
            vr.set_surfxr_controls([sync](surfxr::SettingChange change){return sync->request(change);},[sync]{return sync->exit();});
        }
        surfxr::SettingsPreferences saved_preferences;
        const auto preferences_status=surfxr::load_settings(opts.settings_path,saved_preferences);
        result.settings_load=surfxr::settings_store_status_name(preferences_status);
        if(preferences_status==surfxr::SettingsStoreStatus::Ok)result.settings_adjusted=restore_settings(settings,saved_preferences);
        if(opts.microphone_override)settings.seed_requested({surfxr::SettingId::MicrophoneEnabled,opts.session.microphone_enabled?1.0:0.0});
        const auto initial=settings.snapshot();
        result.initial_preferences=prepare_initial_settings(settings);
        opts.session.initial_vr_framerate=static_cast<unsigned>(initial.at(surfxr::SettingId::VrFramerate).requested);
        opts.session.initial_microphone_volume=initial.at(surfxr::SettingId::MicrophoneVolume).requested;
        opts.session.initial_microphone_active=initial.at(surfxr::SettingId::MicrophoneEnabled).requested!=0;
        opts.session.initial_foveated_streaming=initial.at(surfxr::SettingId::FoveatedStreaming).requested!=0;
        surfxr::ImageQualityPreferences image_quality;
        result.image_quality_load=surfxr::image_quality_store_status_name(surfxr::load_image_quality(opts.image_quality_path,image_quality));
        // A malformed file remains intact until a valid user edit replaces it.
        surfxr::ImageQualityStore image_quality_store(opts.image_quality_path);
        surfxr::InputPreferences input_preferences;
        result.input_preferences_load=surfxr::input_preferences_store_status_name(surfxr::load_input_preferences(opts.input_preferences_path,input_preferences));
        surfxr::InputPreferencesStore input_preferences_store(opts.input_preferences_path);
        std::atomic_bool controllers_enabled{input_preferences.vr_controllers_enabled};
        surfxr::BatteryMonitor battery_monitor;
        battery_monitor.set_sync_raw_battery(input_preferences.sync_raw_battery);
        std::atomic_bool microphone_enabled{opts.session.initial_microphone_active};
        std::optional<bool> requested_mode=opts.start_vr?std::optional<bool>{true}:std::nullopt;
        surfxr::ViewerDecodeTelemetrySlot decode_telemetry;
        result.stage="streaming";result.connection_closed=false;
        ReceiverWorker receiver_worker{result,opts,slots,available,display,input,control,vr,settings,
            controllers_enabled,microphone_enabled,battery_monitor,test_head_motion,decode_telemetry,desktop_demand};
        std::thread worker([&]{receiver_worker.run();});
        struct StopAndJoin {
            std::thread& worker;std::atomic_bool& stop;
            ~StopAndJoin(){if(worker.joinable()){stop.store(true,std::memory_order_relaxed);worker.join();}}
        } stop_and_join{worker,stop_requested};
        // Callback/setup allocations below may throw too. The guard joins the
        // producer before any captured slot, state or VkDevice is destroyed.
        ViewerUi ui_context{result,opts,display,input,control,vr,settings,settings_store,image_quality,image_quality_store,
            input_preferences,input_preferences_store,controllers_enabled,microphone_enabled,battery_monitor,
            test_head_motion,decode_telemetry,mode_commands,requested_mode,desktop_demand};
        auto ui=ui_context.callbacks();
        // Existing workers retain the available CPU set. Only this XR/render
        // thread prefers the strongest known CPU capacity on heterogeneous SoCs.
        result.render_cpu_affinity=surfxr::configure_render_cpu_affinity(opts.available_cpu_affinity);
        try{result.xr=display.run(panels,stop_requested,opts.session.seconds,ui,
                          layout_status==surfxr::LayoutStoreStatus::Ok?&saved_layout:nullptr);}
        catch(const std::exception& error){result.xr_failed=true;result.xr_error=error.what();result.xr=display.statistics();}
        catch(...){result.xr_failed=true;result.xr_error="unknown_xr_failure";result.xr=display.statistics();}
        stop_requested.store(true,std::memory_order_relaxed);
        worker.join();result.connection_closed=true;
        if(ui_context.timing_trace_enabled||ui_context.timing_trace_pending)result.runtime_status->timing(display.timing_snapshot());
        result.settings_store_stats=settings_store.finish();
        result.image_quality_store_stats=image_quality_store.finish();result.final_image_quality=image_quality;
        result.input_preferences_store_stats=input_preferences_store.finish();result.final_input_preferences=input_preferences;
        result.final_battery=battery_monitor.snapshot();battery_monitor.stop();
        result.input_stats=input->statistics();
        result.vr_stats=vr.statistics();result.control_stats=control->statistics();result.final_settings=settings.snapshot();
        if(!opts.layout_path.empty() && !result.xr_failed && !result.worker_failed && result.receiver.exit_code!=1
           && (layout_status==surfxr::LayoutStoreStatus::Ok || layout_status==surfxr::LayoutStoreStatus::Missing)){
            if(const auto layout=display.layout())result.layout_save=surfxr::layout_store_status_name(surfxr::save_layout(opts.layout_path,*layout));
        }
        result.stage="closed";
    }catch(const std::exception& error){
        stop_requested.store(true,std::memory_order_relaxed);result.connection_closed=true;result.xr_failed=true;result.startup_error=error.what();
    }catch(...){
        // No worker escapes the local try above; initialization/thread-creation
        stop_requested.store(true,std::memory_order_relaxed);result.connection_closed=true;result.xr_failed=true;
    }
    return result.print();
}
