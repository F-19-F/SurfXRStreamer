#pragma once
// Test-only scalar reference of the controlled SGSR1 port. The direction-aware
// 12-tap formula is derived from Qualcomm's BSD-3-Clause source; see
// ../frame/xr/shaders/image_enhancement.LICENSE. No Vulkan, XR or video input is used.
#include "surfxr/image_enhancement.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace surfxr::enhancement_reference {
using Color=std::array<float,3>;
struct Image {unsigned width=0,height=0;std::vector<std::uint8_t> rgba;};
inline float linear(float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);}
inline float srgb(float v){return v<=.0031308f?12.92f*std::max(v,0.f):1.055f*std::pow(v,1.f/2.4f)-.055f;}
// Test the shader's performance approximation independently; enhance() below
// deliberately retains exact pow as its image-quality reference.
inline float fast_srgb(float v){
    if(v<=.0031308f)return 12.92f*std::max(v,0.f);
    const float t=std::sqrt(v);
    return std::clamp(((((((.8701133868994889f*t-3.814720686111307f)*t+7.0404575782148155f)*t-7.167729728585985f)*t+4.490841901755987f)*t-1.9472061127437728f)*t+1.5698853928873395f)*t-.041606806257264095f,0.f,1.f);
}
inline float fast_linear(float v){
    if(v<=.04045f)return v/12.92f;
    return std::clamp((((((-.04895767106617191f*v+.20727361103902037f)*v-.4090387700149742f)*v+.7008357119138372f)*v+.5164904585720449f)*v+.03244249830160861f)*v+.0009453002069357841f,0.f,1.f);
}
inline float texel(const Image& image,int x,int y,unsigned c){
    x=std::clamp(x,0,int(image.width)-1);y=std::clamp(y,0,int(image.height)-1);
    return image.rgba[(std::size_t(y)*image.width+x)*4+c]/255.f;
}
inline Color sample(const Image& image,float u,float v){
    const float x=u*image.width-.5f,y=v*image.height-.5f;
    const int ix=int(std::floor(x)),iy=int(std::floor(y));const float fx=x-ix,fy=y-iy;
    Color result{};
    for(unsigned c=0;c<3;++c){const float a=linear(texel(image,ix,iy,c)),b=linear(texel(image,ix+1,iy,c));
        const float d=linear(texel(image,ix,iy+1,c)),e=linear(texel(image,ix+1,iy+1,c));
        result[c]=srgb((a+(b-a)*fx)*(1-fy)+(d+(e-d)*fx)*fy);}
    return result;
}
inline Color enhance(const Image& image,float u,float v,ImageEnhancementOptions options){
    if(!image.width||!image.height||image.rgba.size()!=std::uint64_t{image.width}*image.height*4||!valid_image_enhancement_options(options))
        throw std::invalid_argument("Invalid enhancement reference input");
    const auto base=sample(image,u,v);auto color=base;
    if(options.super_resolution){
        const float x=u*image.width-.5f,y=v*image.height+.5f;
        const int ix=int(std::floor(x)),iy=int(std::floor(y));const float px=x-ix,py=y-iy;
        // Independent integer addressing equivalent to GLSL's gather ordering.
        const auto g=[&](int dx,int dy){return texel(image,ix+dx,iy+dy,1);};
        std::array<float,4> left{{g(-1,0),g(0,0),g(0,-1),g(-1,-1)}};
        const float vote=std::abs(left[2]-left[1])+std::abs(base[1]-left[1])+std::abs(base[1]-left[2]);
        if(vote>4.f/255){
            std::array<float,4> right{{g(1,0),g(2,0),g(2,-1),g(1,-1)}};
            std::array<float,4> ud{{g(0,-2),g(1,-2),g(1,1),g(0,1)}};
            const float mean=(left[1]+left[2]+right[0]+right[3])*.25f;
            float sum=0;for(auto* values:{&left,&right,&ud})for(auto& value:*values){value-=mean;sum+=std::abs(value);}
            const float sm=10.14185f/std::max(sum,1e-6f),standard=sm*sm;
            const float a=right[0]-left[2],b=right[3]-left[1],dx=a+b,dy=a-b;
            const float norm=1/std::sqrt(dx*dx+dy*dy+3.075740e-5f),dirx=dx*norm,diry=dy*norm;
            float weights=0,weighted=0;
            const auto tap=[&](float tx,float ty,float c){
                const float edge=tx*diry+ty*dirx;
                const float q=tx*tx+ty*ty+edge*edge*(std::clamp(c*c*standard,0.f,1.f)*.7f-1);
                const float wa=q-4,weight=(q*wa-wa)*wa*wa;weights+=weight;weighted+=weight*c;};
            tap(px,py+1,ud[0]);tap(px-1,py+1,ud[1]);tap(px-1,py-2,ud[2]);tap(px,py-2,ud[3]);
            tap(px+1,py-1,left[0]);tap(px,py-1,left[1]);tap(px,py,left[2]);tap(px+1,py,left[3]);
            tap(px-1,py-1,right[0]);tap(px-2,py-1,right[1]);tap(px-2,py,right[2]);tap(px-1,py,right[3]);
            if(std::abs(weights)>=1e-6f){
                const float lo=std::min({left[1],left[2],right[0],right[3]}),hi=std::max({left[1],left[2],right[0],right[3]});
                const float delta=std::clamp(std::clamp(2*weighted/weights,lo,hi)-(base[1]-mean),-23.f/255,23.f/255);
                for(auto& value:color)value=std::clamp(value+delta,0.f,1.f);
            }
        }
    }
    if(options.sharpness>0){
        const float n=sample(image,u,v-1.f/image.height)[1],s=sample(image,u,v+1.f/image.height)[1];
        const float w=sample(image,u-1.f/image.width,v)[1],e=sample(image,u+1.f/image.width,v)[1],center=base[1];
        const float lo=std::min({center,n,s,w,e}),hi=std::max({center,n,s,w,e});
        const float amp=std::sqrt(std::clamp(std::min(lo,1-hi)/std::max(hi,1e-6f),0.f,1.f));
        const float weight=-.175f*options.sharpness*amp;
        const float delta=std::clamp((center+weight*(n+s+w+e))/(1+4*weight)-center,-23.f/255,23.f/255);
        for(auto& value:color)value=std::clamp(value+delta,0.f,1.f);
    }
    return color;
}
inline Image fixture(unsigned width,unsigned height,unsigned eye){
    Image image{width,height,std::vector<std::uint8_t>(std::size_t(width)*height*4)};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const unsigned shade=((x/3+y/5+eye)%2)?166:82;const auto p=(std::size_t(y)*width+x)*4;
        image.rgba[p]=static_cast<std::uint8_t>(eye?shade/2:shade+30);
        image.rgba[p+1]=static_cast<std::uint8_t>(shade);
        image.rgba[p+2]=static_cast<std::uint8_t>(eye?shade+50:shade/2);
        image.rgba[p+3]=255;
        if(x==width/3||y==height/4)image.rgba[p+1]=220;
    }
    return image;
}
} // namespace surfxr::enhancement_reference
