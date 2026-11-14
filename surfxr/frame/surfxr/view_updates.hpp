#pragma once
#include "wire.hpp"
#include <cmath>

namespace surfxr::transport {
// Optical control owner, shared by VR and desktop. Observe direction before
// accepting fine movement; never infer user intent from noisy per-frame deltas.
class ViewUpdates {
    TrackingFrame last_;
    std::uint64_t sent_at_=0,motion_at_=0;
    float anchor_=0,run_start_=0;
    unsigned steps_=0;
    int direction_=0;
    bool following_=false;
    void reset_motion(float ipd,std::uint64_t now){
        anchor_=run_start_=ipd;motion_at_=now;steps_=0;direction_=0;following_=false;
    }
public:
    bool should_send(const TrackingFrame& frame,std::uint64_t now){
        if(!frame.optics_valid)return false;
        if(views_changed(frame,last_)){reset_motion(frame.ipd_meters,now);return true;}
        const float step=frame.ipd_meters-anchor_;
        // Quantize observation, not the transmitted value. Head-motion float
        // noise must not create direction evidence at the 360 Hz upload rate.
        if(std::abs(step)>=.00002f){
            const int direction=step>0?1:-1;
            if(direction!=direction_){direction_=direction;run_start_=anchor_;steps_=1;following_=false;}
            else if(steps_<3)++steps_;
            anchor_=frame.ipd_meters;motion_at_=now;
            if(steps_>=3&&std::abs(frame.ipd_meters-run_start_)>=.0001f)following_=true;
        }
        if(following_&&now-motion_at_>400'000'000){
            const bool final=now-sent_at_>=200'000'000&&std::abs(frame.ipd_meters-last_.ipd_meters)>.00005f;
            reset_motion(frame.ipd_meters,now);return final;
        }
        // A single wheel step can span several sensor increments between XR
        // samples. Accept its one-sided displacement only after a quiet hold.
        const bool held=direction_&&std::abs(frame.ipd_meters-run_start_)>=.0001f&&
            now-motion_at_>=300'000'000;
        return now-sent_at_>=200'000'000&&std::abs(frame.ipd_meters-last_.ipd_meters)>.00005f&&
            (following_||held);
    }
    // Call only after the control write succeeds. Repeated poses and queued
    // candidates must not move the transmitted-value or rate-limit baseline.
    void sent(const TrackingFrame& frame,std::uint64_t now){
        last_=frame;sent_at_=now;
        if(!following_)reset_motion(frame.ipd_meters,now);
    }
};
}
