#pragma once
#include "socket.hpp"
#include "tracking_ledger.hpp"
#include "surfxr/session.hpp"
#include "surfxr/surfxr_feedback.hpp"
#include "pose_clock.hpp"
#include <thread>

namespace surfxr::transport {
class Upload {
public:
    Upload(Connection&,TrackingLedger&,const SessionOptions&,const SessionCallbacks&,float hz,bool gaze=false,bool frame_inputs=false,bool frame_skeletons=false,bool boundary_sync=false,bool controller_state=false,std::shared_ptr<PoseClock> pose_clock={});
    ~Upload();
    void check();
private:
    Connection& connection_;TrackingLedger& ledger_;const SessionOptions& options_;const SessionCallbacks& callbacks_;float hz_;bool gaze_,frame_inputs_,frame_skeletons_;
    bool boundary_sync_,controller_state_;
    std::shared_ptr<PoseClock> pose_clock_;
    std::atomic_bool stopping_{false};std::mutex error_mutex_;std::exception_ptr error_;std::thread worker_;
    void run();
};
}
