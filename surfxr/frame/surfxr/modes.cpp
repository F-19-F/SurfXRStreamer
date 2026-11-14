#include "surfxr/surfxr.hpp"
#include "surfxr/surfxr_modes.hpp"
#include "surfxr/vr_wire.hpp"
#include "unified.hpp"
#include "dashboard.hpp"
#include "mode_startup.hpp"
#include <chrono>
#include <thread>

namespace surfxr::transport {
SessionResult run_modes(const SessionOptions& options,const SessionCallbacks& callbacks,const std::atomic_bool* stop){
    auto modes=options.surfxr_modes;
    const auto deadline=options.seconds?std::chrono::steady_clock::now()+std::chrono::seconds(options.seconds):std::chrono::steady_clock::time_point::max();
    SessionResult result;
    unsigned vr_fallbacks=0;std::string last_vr_failure;
    std::shared_ptr<UnifiedTransport> unified=options.surfxr_unified;
    const auto cancelled=[&]{return (stop&&stop->load())||std::chrono::steady_clock::now()>=deadline;};
    try{
        if(!unified&&UnifiedTransport::available(options,cancelled)){
            const auto ticket=request_desktop(options,cancelled);
            require(ticket.unified,"surfxr_unified_host_changed");unified=std::make_shared<UnifiedTransport>(options,callbacks,ticket,stop);
        }
    }catch(const Cancelled&){return result;}
    catch(const std::exception&){result.exit_code=1;result.sanitized_json="{\"backend\":\"surfxr\",\"stage\":\"surfxr_unified\",\"error\":\"surfxr_unified_connect_failed\"}";return result;}
    modes->prepare_vr(bool(unified));
    ModeStartup startup(options,callbacks,modes,cancelled);
    do{
        if((stop&&stop->load())||std::chrono::steady_clock::now()>=deadline)break;
        auto session=options;session.surfxr_modes.reset();session.seconds=0;
        session.surfxr_desktop=modes->begin();session.surfxr_start_prepared=modes->take_prepared();session.surfxr_dashboard_desktop=true;
        session.surfxr_unified=unified;
        if(session.control_queue){SessionControlCommand command;while(session.control_queue->pop(command)){};}
        result=run(session,callbacks,&modes->cancel);
        if(unified)unified->end();
        // Source failure must not tear down a healthy desktop/XR owner. The
        // desktop attempt is bounded by its own existing checks; it is never
        // used after reliable transport failure or ahead of a user request.
        if(unified&&!session.surfxr_desktop&&!modes->pending()&&!cancelled()&&
                !result.server_stopped&&(result.exit_code!=0||result.reconnect_exhausted)){
            try{
                unified->check();
                if(modes->fallback_to_desktop()){
                    last_vr_failure=result.sanitized_json;++vr_fallbacks;
                }
            }catch(const Cancelled&){}
            catch(...){result.exit_code=1;if(callbacks.steamvr_startup)callbacks.steamvr_startup({SteamVrStartupState::Failed,SteamVrStartupReason::TransportFailed,0});}
        }
        // Backend owners have joined uploads/audio and closed sockets; the
        // connection_ended callback has retired decoder sources before reuse.
        if(!modes->pending())break;
    }while(!(stop&&stop->load())&&std::chrono::steady_clock::now()<deadline);
    startup.close();
    if(unified){unified->close();if(!result.sanitized_json.empty())result.sanitized_json.insert(result.sanitized_json.size()-1,",\"unified_session\":"+unified->report());}
    if(vr_fallbacks&&!result.sanitized_json.empty())result.sanitized_json.insert(result.sanitized_json.size()-1,
        ",\"unified_vr_fallbacks\":"+std::to_string(vr_fallbacks)+",\"last_vr_failure\":"+(last_vr_failure.empty()?"null":last_vr_failure));
    return result;
}
}
