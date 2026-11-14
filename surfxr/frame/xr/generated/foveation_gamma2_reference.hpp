#pragma once
#include "frame/xr/generated/image_enhancement_reference.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// Test-only scalar reference. Start with decoded D bytes, decode each texel to
// linear light, then bilerp. In particular, do not derive q from a quantized E
// readback: the production Gamma2 path has no intermediate 8-bit E image.
namespace surfxr::gamma2_reference {
using Color=std::array<double,3>;
using Image=enhancement_reference::Image;
inline double encode(double linear){
    linear=std::clamp(linear,0.0,1.0);
    return linear<=.0031308?12.92*linear:1.055*std::pow(linear,1.0/2.4)-.055;
}
inline double decode(unsigned byte){
    const double code=byte/255.0;
    return code<=.04045?code/12.92:std::pow((code+.055)/1.055,2.4);
}
inline double coordinate(double t){
    if(t<.25)return 2*t*t;
    if(t<.75)return 1.5*t-.25;
    return 1-2*(1-t)*(1-t);
}
inline Color sample_q(const Image& image,unsigned width,unsigned height,int px,int py){
    const double u=coordinate((std::clamp(px,0,int(width)-1)+.5)/width);
    const double v=coordinate((std::clamp(py,0,int(height)-1)+.5)/height);
    const double x=u*image.width-.5,y=v*image.height-.5;
    const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const double fx=x-ix,fy=y-iy;
    const auto texel=[&](int tx,int ty,unsigned c){
        tx=std::clamp(tx,0,int(image.width)-1);ty=std::clamp(ty,0,int(image.height)-1);
        return decode(image.rgba[(std::size_t(ty)*image.width+unsigned(tx))*4+c]);
    };
    Color q{};
    for(unsigned c=0;c<3;++c){
        const double top=texel(ix,iy,c)*(1-fx)+texel(ix+1,iy,c)*fx;
        const double bottom=texel(ix,iy+1,c)*(1-fx)+texel(ix+1,iy+1,c)*fx;
        q[c]=std::sqrt(std::max(0.0,top*(1-fy)+bottom*fy));
    }
    return q;
}
inline double proxy(double q){
    if(q<=.05595355216606)return 12.92*q*q;
    if(q>=1)return 1;
    return std::clamp(.19876842200756073*q*q*q-.52150958776474*q*q+
                      1.3565999269485474*q-.03385872393846512,0.0,1.0);
}
inline double cap(const Color& q){
    const double low=std::max(*std::min_element(q.begin(),q.end())-.10259270,.05595355216606);
    const double inverse_slope=1.0546282662630735*low*low*low-2.129381314802799*low*low+
                               1.6043986913567392*low+.6077911154768247;
    return std::min((23.0/255)*std::max(1.0/1.447,inverse_slope),.10259269);
}
struct Sample {Color encoded{},unsharpened{};double delta=0,limit=0;};
inline Sample sharpen(const Image& image,unsigned width,unsigned height,int x,int y,double strength){
    const Color q=sample_q(image,width,height,x,y);
    const std::array<double,4> neighbors{{sample_q(image,width,height,x,y-1)[1],
        sample_q(image,width,height,x,y+1)[1],sample_q(image,width,height,x-1,y)[1],
        sample_q(image,width,height,x+1,y)[1]}};
    double low=q[1],high=q[1],highpass=4*q[1];
    for(double green:neighbors){low=std::min(low,green);high=std::max(high,green);highpass-=green;}
    low=proxy(low);high=proxy(high);
    const double amplitude=std::min(low,1-high),weighted=.175*strength*amplitude;
    Sample result;result.limit=cap(q);
    result.delta=std::clamp(weighted*highpass/std::max(std::sqrt(high*amplitude)-4*weighted,1e-6),
                            -result.limit,result.limit);
    for(unsigned c=0;c<3;++c){
        const double enhanced=std::clamp(q[c]+result.delta,0.0,1.0);
        result.encoded[c]=encode(enhanced*enhanced);
        result.unsharpened[c]=encode(q[c]*q[c]);
    }
    return result;
}
struct CoefficientChecks {double proxy_max_error=0,halo_max_codes=0;bool monotonic=true,endpoints=true;};
struct Interval {double low=0,high=0;};
using ColorInterval=std::array<Interval,3>;
// A hardware-aware verification envelope, separate from the ideal reference.
// The device reports subTexelPrecisionBits for coordinate filtering. In the
// Frame float readback, sampled linear RGB also differs from ideal decoding /
// filtering by up to roughly .00028; 1/2048 bounds that observed discrepancy.
// The latter is a declared fixture allowance, not a Vulkan accuracy guarantee.
inline ColorInterval sample_q_interval(const Image& image,unsigned width,unsigned height,int px,int py,
                                      unsigned subtexel_bits,double linear_error){
    const double x=coordinate((std::clamp(px,0,int(width)-1)+.5)/width)*image.width-.5;
    const double y=coordinate((std::clamp(py,0,int(height)-1)+.5)/height)*image.height-.5;
    const double uncertainty=std::ldexp(1.0,-int(subtexel_bits));
    const auto axis=[&](double center){
        std::vector<double> values{center-uncertainty,center+uncertainty};
        for(double boundary=std::ceil(values[0]);boundary<values[1];boundary+=1)values.push_back(boundary);
        return values;
    };
    const auto xs=axis(x),ys=axis(y);
    ColorInterval result{{{1,0},{1,0},{1,0}}};
    const auto texel=[&](int tx,int ty,unsigned c){
        tx=std::clamp(tx,0,int(image.width)-1);ty=std::clamp(ty,0,int(image.height)-1);
        return decode(image.rgba[(std::size_t(ty)*image.width+unsigned(tx))*4+c]);
    };
    // Bilinear extrema on each cell lie at its corners. Include integer
    // boundaries so this remains valid when the uncertainty crosses a cell.
    for(double sy:ys)for(double sx:xs){
        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const double fx=sx-ix,fy=sy-iy;
        for(unsigned c=0;c<3;++c){
            const double value=(texel(ix,iy,c)*(1-fx)+texel(ix+1,iy,c)*fx)*(1-fy)+
                               (texel(ix,iy+1,c)*(1-fx)+texel(ix+1,iy+1,c)*fx)*fy;
            result[c].low=std::min(result[c].low,value);result[c].high=std::max(result[c].high,value);
        }
    }
    for(auto& c:result){c.low=std::sqrt(std::max(0.0,c.low-linear_error));c.high=std::sqrt(std::min(1.0,c.high+linear_error));}
    return result;
}
inline ColorInterval sharpen_interval(const Image& image,unsigned width,unsigned height,int x,int y,double strength,
                                     unsigned subtexel_bits,double linear_error){
    const auto base=sample_q_interval(image,width,height,x,y,subtexel_bits,linear_error);
    const std::array<Interval,4> neighbors{{sample_q_interval(image,width,height,x,y-1,subtexel_bits,linear_error)[1],
        sample_q_interval(image,width,height,x,y+1,subtexel_bits,linear_error)[1],
        sample_q_interval(image,width,height,x-1,y,subtexel_bits,linear_error)[1],
        sample_q_interval(image,width,height,x+1,y,subtexel_bits,linear_error)[1]}};
    Interval low=base[1],high=base[1],highpass{4*base[1].low,4*base[1].high};
    for(const auto& n:neighbors){
        low.low=std::min(low.low,n.low);low.high=std::min(low.high,n.high);
        high.low=std::max(high.low,n.low);high.high=std::max(high.high,n.high);
        highpass.low-=n.high;highpass.high-=n.low;
    }
    low={proxy(low.low),proxy(low.high)};high={proxy(high.low),proxy(high.high)};
    const Interval t{std::max(0.0,std::min(low.low,1-high.high)),std::max(0.0,std::min(low.high,1-high.low))};
    // Use the original CAS weight form, independently of the product-form
    // shader algebra. t<=hi bounds sqrt(t/hi) to [0,1], hence 1-4*a>=.3.
    const double a_low=high.high>0?.175*strength*std::sqrt(t.low/high.high):0;
    const double a_high=high.low>0?.175*strength*std::sqrt(std::min(1.0,t.high/high.low)):.175*strength;
    const double k_low=a_low/(1-4*a_low),k_high=a_high/(1-4*a_high);
    const std::array<double,4> products{{k_low*highpass.low,k_low*highpass.high,k_high*highpass.low,k_high*highpass.high}};
    Interval delta{*std::min_element(products.begin(),products.end()),*std::max_element(products.begin(),products.end())};
    // The shader's denominator floor can only move this result toward zero.
    if(std::sqrt(high.low*t.low)-.7*strength*t.high<1e-6){delta.low=std::min(delta.low,0.0);delta.high=std::max(delta.high,0.0);}
    const double cap_low=cap({base[0].low,base[1].low,base[2].low});
    const double cap_high=cap({base[0].high,base[1].high,base[2].high});
    const std::array<double,4> capped{{std::clamp(delta.low,-cap_low,cap_low),std::clamp(delta.low,-cap_high,cap_high),
        std::clamp(delta.high,-cap_low,cap_low),std::clamp(delta.high,-cap_high,cap_high)}};
    delta={*std::min_element(capped.begin(),capped.end()),*std::max_element(capped.begin(),capped.end())};
    ColorInterval result;
    for(unsigned c=0;c<3;++c){
        const double lo=std::clamp(base[c].low+delta.low,0.0,1.0),hi=std::clamp(base[c].high+delta.high,0.0,1.0);
        result[c]={encode(lo*lo),encode(hi*hi)};
    }
    return result;
}
inline CoefficientChecks check_coefficients(){
    CoefficientChecks result;result.endpoints=proxy(0)==0&&proxy(1)==1;
    double previous=0;
    // Dense scalar checks include the nonlinear toe and both delta signs.
    // These are numerical regressions, not a proof for every floating input.
    for(unsigned i=0;i<=100000;++i){
        const double q=i/100000.0,value=proxy(q);
        result.monotonic&=value>=previous;previous=value;
        result.proxy_max_error=std::max(result.proxy_max_error,std::abs(value-encode(q*q)));
        const double limit=cap({q,q,q});
        for(double sign:{-1.0,1.0}){
            const double enhanced=std::clamp(q+sign*limit,0.0,1.0);
            result.halo_max_codes=std::max(result.halo_max_codes,255*std::abs(encode(enhanced*enhanced)-encode(q*q)));
        }
    }
    return result;
}
} // namespace surfxr::gamma2_reference
