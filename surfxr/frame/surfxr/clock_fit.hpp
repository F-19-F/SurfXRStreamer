#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>
#include <utility>

namespace surfxr::transport {
struct ClockExchange {std::uint64_t t1,t2,t3,t4;};
// S(C) = server_anchor + (C-client_anchor)*(1+skew) + offset.
// Nonnegative one-way delays give TWO inequalities per exchange. No equal
// forward/reverse delay is assumed. Each clock uses its own local anchor.
struct ClockFit {
    struct Point {double offset,skew;};
    std::uint64_t client_anchor=0,server_anchor=0;
    double offset=0,skew=0,skew_low=0,skew_high=0,min_rtt_ns=0;
    std::vector<Point> feasible;
    static constexpr double precision=1e-6; // software-clock precision floor
    static constexpr double maximum_skew=.005; // incompatible units beyond 5000 ppm
    static double seconds(std::uint64_t a,std::uint64_t b){return (std::int64_t(a)-std::int64_t(b))*1e-9;}
    static void clip(std::vector<Point>& polygon,double a,double b,double c){
        if(polygon.empty())return;
        std::vector<Point> output;output.reserve(polygon.size()+1);
        auto previous=polygon.back();auto pv=a*previous.offset+b*previous.skew-c;
        for(const auto current:polygon){const auto cv=a*current.offset+b*current.skew-c;
            if((pv<=0)!=(cv<=0)){const auto t=pv/(pv-cv);
                output.push_back({previous.offset+t*(current.offset-previous.offset),previous.skew+t*(current.skew-previous.skew)});}
            if(cv<=0)output.push_back(current);
            previous=current;pv=cv;
        }
        polygon=std::move(output);
    }
    static std::optional<ClockFit> fit(const std::deque<ClockExchange>& samples){
        if(samples.empty())return {};
        ClockFit f;f.client_anchor=samples.back().t4;f.server_anchor=samples.back().t3;
        f.feasible={{-1,-maximum_skew},{1,-maximum_skew},{1,maximum_skew},{-1,maximum_skew}};
        for(const auto& s:samples){
            const auto up=seconds(s.t1,f.client_anchor),down=seconds(s.t4,f.client_anchor);
            // S(t1) <= t2 and S(t4) >= t3, including timestamp precision.
            clip(f.feasible,1,up,seconds(s.t2,f.server_anchor)-up+precision);
            clip(f.feasible,-1,-down,-seconds(s.t3,f.server_anchor)+down+precision);
            if(f.feasible.empty())return {};
        }
        f.skew_low=maximum_skew;f.skew_high=-maximum_skew;
        for(const auto p:f.feasible){f.skew_low=std::min(f.skew_low,p.skew);f.skew_high=std::max(f.skew_high,p.skew);}
        f.skew=(f.skew_low+f.skew_high)/2;
        double lower=-1,upper=1;f.min_rtt_ns=1e12;
        for(const auto& s:samples){
            lower=std::max(lower,seconds(s.t3,f.server_anchor)-seconds(s.t4,f.client_anchor)*(1+f.skew)-precision);
            upper=std::min(upper,seconds(s.t2,f.server_anchor)-seconds(s.t1,f.client_anchor)*(1+f.skew)+precision);
            f.min_rtt_ns=std::min(f.min_rtt_ns,std::max(0.,double(s.t4-s.t1)-double(s.t3-s.t2)/(1+f.skew)));
        }
        if(lower>upper+1e-12)return {};
        f.offset=(lower+upper)/2;return f;
    }
    std::int64_t map(std::uint64_t client)const{
        return std::int64_t(server_anchor)+std::llround((seconds(client,client_anchor)*(1+skew)+offset)*1e9);
    }
    std::pair<std::int64_t,std::int64_t> bounds(std::uint64_t client)const{
        const auto dt=seconds(client,client_anchor);double low=1e30,high=-1e30;
        for(const auto p:feasible){const auto value=(dt*(1+p.skew)+p.offset)*1e9;
            low=std::min(low,value);high=std::max(high,value);}
        return {std::int64_t(server_anchor)+std::int64_t(std::floor(low)),
                std::int64_t(server_anchor)+std::int64_t(std::ceil(high))};
    }
    std::uint64_t uncertainty(std::uint64_t client,std::int64_t mapped)const{
        const auto dt=seconds(client,client_anchor),center=(mapped-std::int64_t(server_anchor))*1e-9;
        double error=0;
        for(const auto p:feasible)error=std::max(error,std::abs(dt*(1+p.skew)+p.offset-center));
        return std::uint64_t(std::ceil(error*1e9))+1;
    }
};
} // namespace surfxr::transport
