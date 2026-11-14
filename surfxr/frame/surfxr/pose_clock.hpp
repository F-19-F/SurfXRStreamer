#pragma once
#include "surfxr/surfxr_pose.hpp"
#include "clock_fit.hpp"
#include <limits>
#include <mutex>
#include <stdexcept>

namespace surfxr::transport {
// One session, one continuous timeline. Quality is telemetry, not liveness.
// Queue-expanded intervals leave the mapping alone; missing replies coast.
class PoseClock {
public:
    struct Status {
        bool ready=false;
        std::uint64_t samples=0,accepted=0,rejected=0,revision=0,error_ns=0,reply_age_ns=0,min_rtt_ns=0;
        std::int64_t offset_ns=0;
        std::uint64_t rate_ppb=0,rate_low_ppb=0,rate_high_ppb=0;
        std::uint64_t t1=0,t2=0,t3=0,t4=0;
    };
    Bytes request(std::uint64_t now){
        std::lock_guard<std::mutex> lock(mutex_);
        const auto interval=ready_||pending_.size()>=8?250'000'000u:25'000'000u;
        if(last_request_&&now>=last_request_&&now-last_request_<interval)return {};
        last_request_=now;pending_.push_back(now);while(pending_.size()>8)pending_.pop_front();
        return pose_wire::request(now);
    }
    bool observe(const Bytes& bytes,std::uint64_t received){
        std::array<std::uint64_t,4> values{};
        std::lock_guard<std::mutex> lock(mutex_);
        const auto reject=[&]{++rejected_;return false;};
        if(!pose_wire::response(bytes,values))return reject();
        const auto [generation,sent,server_received,server_sent]=values;
        const auto it=std::find(pending_.begin(),pending_.end(),sent);
        if(it==pending_.end())return reject();
        pending_.erase(it);
        constexpr auto limit=std::uint64_t(std::numeric_limits<std::int64_t>::max()/4);
        if(!generation||!sent||sent>received||received>limit||!server_received||server_received>server_sent||
           server_sent>limit||(generation_&&generation!=generation_)||(last_reply_&&received<last_reply_))return reject();
        const auto elapsed=received-sent,processing=server_sent-server_received;
        if(double(processing)>double(elapsed)*(1+ClockFit::maximum_skew)+2000)return reject();
        auto samples=samples_;samples.push_back({sent,server_received,server_sent,received});
        while(samples.size()>64||received-samples.front().t4>16'000'000'000ULL)samples.pop_front();
        auto fit=ClockFit::fit(samples);if(!fit)return reject();
        const auto desired=fit->map(received);
        if(!ready_){
            if(desired<=0)return reject();
            // One measured exchange establishes the epoch. Frequency refines
            // in the background; neither sample count nor a forecast delays VR.
            anchor_client_=received;anchor_server_=desired;base_rate_=1+fit->skew;ready_=true;
        }else{
            const auto current=map(received);
            const auto [low,high]=fit->bounds(received);
            const auto rate=std::clamp(base_rate_,1+fit->skew_low,1+fit->skew_high);
            const auto target=std::clamp(current,low,high);
            // Do not chase a midpoint moved by asymmetric packet queues.
            // Only evidence excluding our mapping may discipline it, with no
            // phase step. A finite slew cannot run forever during reply loss.
            if(rate!=base_rate_||target!=current){
                anchor_client_=received;anchor_server_=current;base_rate_=rate;
                correction_=std::clamp((target-current)*1e-9/2.,-.001,.001);
                correction_end_=received+2'000'000'000;
            }
        }
        samples_=std::move(samples);fit_=std::move(fit);generation_=generation;
        last_reply_=received;++revision_;++accepted_;return true;
    }
    std::optional<pose_wire::Stamp> stamp(std::uint64_t sampled,std::uint64_t now,
                                        std::uint64_t epoch,std::uint64_t sequence){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!epoch||!sequence||!sampled||sampled>std::uint64_t(std::numeric_limits<std::int64_t>::max()/4)||sampled>now||now-sampled>pose_wire::max_age_ns||
           !ready_||now<last_reply_)return {};
        const auto mapped=map(sampled);if(mapped<=0)return {};
        const auto error=fit_->uncertainty(sampled,mapped);
        return pose_wire::Stamp{generation_,epoch,sequence,std::uint64_t(mapped),sampled,
            revision_,error,std::uint64_t(std::llround(rate_at(sampled)*1e9)),last_reply_};
    }
    Status status(std::uint64_t now){
        std::lock_guard<std::mutex> lock(mutex_);Status s;
        s.ready=ready_;s.samples=samples_.size();s.accepted=accepted_;s.rejected=rejected_;s.revision=revision_;
        if(!samples_.empty()){const auto& e=samples_.back();s.t1=e.t1;s.t2=e.t2;s.t3=e.t3;s.t4=e.t4;}
        if(last_reply_&&now>=last_reply_)s.reply_age_ns=now-last_reply_;
        if(ready_){const auto mapped=map(now);s.offset_ns=mapped-std::int64_t(now);s.error_ns=fit_->uncertainty(now,mapped);
            s.rate_ppb=std::llround(rate_at(now)*1e9);s.rate_low_ppb=std::llround((1+fit_->skew_low)*1e9);
            s.rate_high_ppb=std::llround((1+fit_->skew_high)*1e9);s.min_rtt_ns=std::llround(fit_->min_rtt_ns);}
        return s;
    }
private:
    std::int64_t map(std::uint64_t client)const{
        const auto dt=std::int64_t(client)-std::int64_t(anchor_client_);
        const auto slew=std::min(dt,std::int64_t(correction_end_)-std::int64_t(anchor_client_));
        return anchor_server_+std::llround(dt*base_rate_+(correction_?slew*correction_:0));
    }
    double rate_at(std::uint64_t client)const{return base_rate_+(client<correction_end_?correction_:0);}
    std::mutex mutex_;std::deque<std::uint64_t> pending_;std::deque<ClockExchange> samples_;
    std::optional<ClockFit> fit_;
    std::uint64_t last_request_=0,last_reply_=0,generation_=0,revision_=0,accepted_=0,rejected_=0,anchor_client_=0,correction_end_=0;
    std::int64_t anchor_server_=0;double base_rate_=1,correction_=0;bool ready_=false;
};
} // namespace surfxr::transport
