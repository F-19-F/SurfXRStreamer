#version 450
// SGSR1 direction kernel adapted from Qualcomm's official BSD-3-Clause source:
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// Full redistribution terms and immutable source URL: image_enhancement.LICENSE
// Controlled port: Vulkan push constants, full-float arithmetic, sRGB-domain
// filtering of sRGB views, VR threshold 4/255, finite-denominator guards.
// Additional 5-tap sharpening is independent and is not Quest compositor 0x8.
layout(set=0,binding=0) uniform sampler2D complete_eye;
// Compatible UNORM view of the same image; reads encoded values without the
// hardware EOTF. Never use it for bilinear filtering of the upscaled image.
layout(set=0,binding=1) uniform sampler2D encoded_eye;
// Image view interpretation and target size stay fixed for a pipeline. This
// lets the driver remove SGSR and transfer functions from native sharpening,
// instead of assigning registers for an unreachable, much larger kernel.
layout(constant_id=0) const bool SOURCE_ENCODED=false;
layout(constant_id=1) const bool TARGET_ENCODED=false;
layout(constant_id=2) const bool UPSCALE=false;
layout(push_constant) uniform Parameters {vec4 viewport;vec4 options;} params;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 output_color;

// Bounded polynomial approximation to the standard sRGB transfer functions.
// Independent CPU tests scan the complete [0,1] domain against exact pow:
// OETF error <= .00042 (0.108 of an 8-bit level), EOTF <= .00002 linear.
// Dark linear segments are exact. Keep full precision; half arithmetic can
// overflow the directional kernel and magnify cancellation in these polynomials.
#define ENCODE(t) (((((((.8701133868994889*(t)-3.814720686111307)*(t)+7.0404575782148155)*(t)-7.167729728585985)*(t)+4.490841901755987)*(t)-1.9472061127437728)*(t)+1.5698853928873395)*(t)-.041606806257264095)
#define DECODE(v) ((((((-.04895767106617191*(v)+.20727361103902037)*(v)-.4090387700149742)*(v)+.7008357119138372)*(v)+.5164904585720449)*(v)+.03244249830160861)*(v)+.0009453002069357841)
vec3 to_srgb(vec3 v){v=max(v,vec3(0));vec3 t=sqrt(v);return clamp(mix(ENCODE(t),12.92*v,lessThanEqual(v,vec3(.0031308))),0.0,1.0);}
vec4 to_srgb(vec4 v){v=max(v,vec4(0));vec4 t=sqrt(v);return clamp(mix(ENCODE(t),12.92*v,lessThanEqual(v,vec4(.0031308))),0.0,1.0);}
float to_srgb(float v){v=max(v,0);float t=sqrt(v);return clamp(v<=.0031308?12.92*v:ENCODE(t),0.0,1.0);}
vec3 to_linear(vec3 v){return clamp(mix(DECODE(v),v/12.92,lessThanEqual(v,vec3(.04045))),0.0,1.0);}
float fast_lanczos2(float x){float a=x-4.0;float b=x*a-a;return b*a*a;}
vec2 weight_y(float dx,float dy,float c,vec3 data){
    float edge_distance=dx*data.z+dy*data.y;
    float x=dx*dx+dy*dy+edge_distance*edge_distance*(clamp(c*c*data.x,0.0,1.0)*.7-1.0);
    float w=fast_lanczos2(x);return vec2(w,w*c);
}
vec2 edge_direction(vec4 left,vec4 right){
    float a=right.x-left.z,b=right.w-left.y;
    vec2 delta=vec2(a+b,a-b);
    return delta*inversesqrt(dot(delta,delta)+3.075740e-05);
}
float sharpen_delta(float center,vec4 neighbors){
    float lo=min(center,min(min(neighbors.x,neighbors.y),min(neighbors.z,neighbors.w)));
    float hi=max(center,max(max(neighbors.x,neighbors.y),max(neighbors.z,neighbors.w)));
    float amplitude=sqrt(clamp(min(lo,1.0-hi)/max(hi,1e-6),0.0,1.0));
    float weight=-.175*params.options.y*amplitude;
    float sharpened=(center+weight*dot(neighbors,vec4(1)))/(1.0+4.0*weight);
    return clamp(sharpened-center,-23.0/255.0,23.0/255.0);
}
vec3 sgsr(vec3 base,out float sharpening_delta,out bool shared_sharpening){
    sharpening_delta=0;shared_sharpening=false;
    vec2 image_coord=uv*params.viewport.zw+vec2(-.5,.5);
    vec2 pixel=floor(image_coord),coord=pixel*params.viewport.xy,pl=image_coord-pixel;
    vec4 left_linear=vec4(0);
    vec4 left;
    if(SOURCE_ENCODED)left=textureGather(encoded_eye,coord,1);
    else {left_linear=textureGather(complete_eye,coord,1);left=to_srgb(left_linear);}
    float vote=abs(left.z-left.y)+abs(base.g-left.y)+abs(base.g-left.z);
    if(vote<=4.0/255.0)return base;
    coord.x+=params.viewport.x;
    vec4 right_linear=vec4(0),up_down_linear=vec4(0),right,up_down;
    if(SOURCE_ENCODED){
        right=textureGather(encoded_eye,coord+vec2(params.viewport.x,0),1);
        up_down.xy=textureGather(encoded_eye,coord+vec2(0,-params.viewport.y),1).wz;
        up_down.zw=textureGather(encoded_eye,coord+vec2(0,params.viewport.y),1).yx;
    }else{
        right_linear=textureGather(complete_eye,coord+vec2(params.viewport.x,0),1);
        up_down_linear.xy=textureGather(complete_eye,coord+vec2(0,-params.viewport.y),1).wz;
        up_down_linear.zw=textureGather(complete_eye,coord+vec2(0,params.viewport.y),1).yx;
        right=to_srgb(right_linear);up_down=to_srgb(up_down_linear);
    }
    if(params.options.y>0&&!SOURCE_ENCODED){
        // These 12 gather texels are exactly the union of the four bilinear
        // neighbor footprints. Reuse linear values before OETF to reproduce
        // textureLodOffset filtering, saving four additional texture fetches.
        vec4 neighbors;
        neighbors.x=mix(mix(up_down_linear.x,up_down_linear.y,pl.x),mix(left_linear.z,right_linear.w,pl.x),pl.y);
        neighbors.y=mix(mix(left_linear.y,right_linear.x,pl.x),mix(up_down_linear.w,up_down_linear.z,pl.x),pl.y);
        neighbors.z=mix(mix(left_linear.w,left_linear.z,pl.x),mix(left_linear.x,left_linear.y,pl.x),pl.y);
        neighbors.w=mix(mix(right_linear.w,right_linear.z,pl.x),mix(right_linear.x,right_linear.y,pl.x),pl.y);
        sharpening_delta=sharpen_delta(base.g,to_srgb(neighbors));shared_sharpening=true;
    }
    float mean=(left.y+left.z+right.x+right.w)*.25;
    left-=mean;right-=mean;up_down-=mean;
    float sum=dot(abs(left)+abs(right)+abs(up_down),vec4(1));
    float sum_mean=10.14185/max(sum,1e-6);
    vec3 data=vec3(sum_mean*sum_mean,edge_direction(left,right));
    vec2 a=weight_y(pl.x,pl.y+1,up_down.x,data);
    a+=weight_y(pl.x-1,pl.y+1,up_down.y,data);
    a+=weight_y(pl.x-1,pl.y-2,up_down.z,data);
    a+=weight_y(pl.x,pl.y-2,up_down.w,data);
    a+=weight_y(pl.x+1,pl.y-1,left.x,data);
    a+=weight_y(pl.x,pl.y-1,left.y,data);
    a+=weight_y(pl.x,pl.y,left.z,data);
    a+=weight_y(pl.x+1,pl.y,left.w,data);
    a+=weight_y(pl.x-1,pl.y-1,right.x,data);
    a+=weight_y(pl.x-2,pl.y-1,right.y,data);
    a+=weight_y(pl.x-2,pl.y,right.z,data);
    a+=weight_y(pl.x-1,pl.y,right.w,data);
    if(abs(a.x)<1e-6)return base;
    float minimum=min(min(left.y,left.z),min(right.x,right.w));
    float maximum=max(max(left.y,left.z),max(right.x,right.w));
    float delta=clamp(2.0*a.y/a.x,minimum,maximum)-(base.g-mean);
    return clamp(base+clamp(delta,-23.0/255.0,23.0/255.0),0.0,1.0);
}
float independent_sharpen(float center){
    float n=to_srgb(textureLodOffset(complete_eye,uv,0,ivec2(0,-1)).g);
    float s=to_srgb(textureLodOffset(complete_eye,uv,0,ivec2(0,1)).g);
    float w=to_srgb(textureLodOffset(complete_eye,uv,0,ivec2(-1,0)).g);
    float e=to_srgb(textureLodOffset(complete_eye,uv,0,ivec2(1,0)).g);
    return sharpen_delta(center,vec4(n,s,w,e));
}
vec3 encoded_texel(ivec2 p){
    return texelFetch(encoded_eye,clamp(p,ivec2(0),ivec2(params.viewport.zw)-1),0).rgb;
}
vec3 sharpen_native(){
    // At equal source/target size, every sample is exactly on a texel center.
    // Reading the stored code values preserves the filtering domain without
    // a hardware EOTF followed by a software OETF for each of the five taps.
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec3 base=encoded_texel(p);
    vec4 neighbors=vec4(encoded_texel(p+ivec2(0,-1)).g,encoded_texel(p+ivec2(0,1)).g,
                        encoded_texel(p+ivec2(-1,0)).g,encoded_texel(p+ivec2(1,0)).g);
    return clamp(base+sharpen_delta(base.g,neighbors),0.0,1.0);
}
void main(){
    if(SOURCE_ENCODED&&!UPSCALE){
        vec3 enhanced=sharpen_native();
        output_color=vec4(TARGET_ENCODED?enhanced:to_linear(enhanced),1);
        return;
    }
    vec3 base=to_srgb(textureLod(complete_eye,uv,0).rgb);
    float sharpening_delta=0;bool shared_sharpening=false;
    vec3 enhanced=UPSCALE?sgsr(base,sharpening_delta,shared_sharpening):base;
    if(params.options.y>0){
        if(!shared_sharpening)sharpening_delta=independent_sharpen(base.g);
        enhanced=clamp(enhanced+sharpening_delta,0.0,1.0);
    }
    // A UNORM view stores the intended code values directly. The fallback
    // sRGB attachment performs OETF once. Alpha is intentionally opaque.
    output_color=vec4(TARGET_ENCODED?enhanced:to_linear(enhanced),1);
}
