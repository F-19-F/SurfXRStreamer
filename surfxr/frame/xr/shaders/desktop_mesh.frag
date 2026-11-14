#version 450
#extension GL_GOOGLE_include_directive : require
// Match the vertex-stage specialization for projection and cache conversion.
layout(constant_id=0) const uint render_mode=0u;
layout(set=0,binding=0) uniform sampler2D desktop;
layout(set=0,binding=1) uniform sampler2D chroma;
layout(location=0) in vec2 uv;
layout(location=1) in vec3 facing_normal;
layout(location=2) in vec3 to_eye;
layout(location=0) out vec4 color;
layout(push_constant) uniform Camera {mat4 vp;vec4 eye;vec4 sampling;} camera;
#include "nv12_sampling.glsl"
void main(){
    if(render_mode>=3u){
        vec3 code=sample_nv12_code(desktop,chroma,uv,camera.vp[0],camera.vp[1]);
        vec2 pixel=floor(gl_FragCoord.xy),extent=camera.vp[2].xy;
        float edge=min(min(pixel.x,pixel.y),min(extent.x-1.0-pixel.x,extent.y-1.0-pixel.y));
        float alpha=edge<1.0?64.0/255.0:(edge<2.0?192.0/255.0:1.0);
        // Baseline owns only mip0, keeps unpremultiplied RGB and the original
        // bilinear sampler. The encoded attachment needs no transfer function.
        if(render_mode==5u){color=vec4(code,alpha);return;}
        if(render_mode==6u){color=vec4(nv12_linear_from_code(code),alpha);return;}
        if(render_mode==4u){
            if(edge<2.0)code=round(code*255.0)/255.0;
            color=vec4(nv12_linear_from_code(code)*alpha,alpha);
            return;
        }
        // This pipeline's compatible UNORM view stores encoded bytes directly.
        // Opaque pixels bypass both transfer functions. Only the two-pixel
        // feather needs linear premultiplication and the legacy RGB8 step.
        if(edge<2.0){
            vec3 linear=nv12_linear_from_code(round(code*255.0)/255.0)*alpha;
            code=mix(1.055*pow(linear,vec3(1.0/2.4))-.055,12.92*linear,lessThanEqual(linear,vec3(.0031308)));
        }
        color=vec4(code,alpha);
        return;
    }
    if(render_mode==2u){
        vec4 source=texelFetch(desktop,ivec2(gl_FragCoord.xy),0);
        // sRGB view decodes to linear light; alpha is already linear. Store
        // premultiplied linear RGB through an sRGB attachment for mip filtering.
        color=vec4(source.rgb*source.a,source.a);
        return;
    }
    // Perspective-correct derivatives precede discard or any varying branch.
    // They describe this projection target, before runtime distortion/late warp.
    // The fixed mip0 specialization has no derivative or mesh-facing work.
    vec2 gx=dFdx(uv),gy=dFdy(uv);
    if(dot(facing_normal,to_eye)<=0.0)discard;
    bool quality=render_mode==1u;
    // Quality uses the complete pixel footprint and trilinear mip sampling.
    // A fixed filter avoids introducing a display-phase change into static
    // text. Magnification remains ordinary level-zero bilinear sampling.
    vec4 pixel=textureGrad(desktop,uv,gx,gy);
    // Silhouette AA stays separate from content filtering. There are no
    // coverage seams between mesh strips, nor a second premultiplication.
    vec2 edge=min(uv,vec2(1.0)-uv);
    vec2 coverage=clamp(edge/max(abs(gx)+abs(gy),vec2(1e-7)),0.0,1.0);
    float silhouette=min(coverage.x,coverage.y);
    if(quality)pixel*=silhouette;else pixel.a*=silhouette;
    if(pixel.a<=0.0001)discard;
    color=pixel;
}
