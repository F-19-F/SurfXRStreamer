// Shared complete-eye sampling. Both shader variants use the same E grid;
// the selected sharpening algorithm declares its own working color domain.
#include "foveation_gamma2.glsl"
vec3 sample_region(sampler2D image,vec2 eye_uv,vec4 rect){
    if(params.yuv.x!=0.0)return nv12_linear_from_code(sample_nv12_code(image,chroma_eye,eye_uv,rect,params.yuv));
    vec2 edge=.5/vec2(textureSize(image,0));
    vec2 at=clamp(rect.xy+eye_uv*rect.zw,rect.xy+edge,rect.xy+rect.zw-edge);
    return textureLod(image,at,0).rgb;
}
vec3 sample_eye(vec2 eye_uv){return sample_region(encoded_eye,eye_uv,params.source_rect);}
float encoded_coordinate(float original){
    if(params.options.w!=0.0)return original;
    if(original<0.25)return 2.0*original*original;
    if(original<0.75)return 1.5*original-0.25;
    float remaining=1.0-original;
    return 1.0-2.0*remaining*remaining;
}
// Use the transfer definition before the intermediate 8-bit quantization.
// Fixed-function sRGB attachment conversion may still differ by one code;
// strong sharpening can amplify that rounding difference.
vec3 to_srgb(vec3 v){v=max(v,vec3(0));return clamp(mix(1.055*pow(v,vec3(1.0/2.4))-.055,12.92*v,lessThanEqual(v,vec3(.0031308))),0.0,1.0);}
float to_srgb(float v){v=max(v,0);return clamp(v<=.0031308?12.92*v:1.055*pow(v,1.0/2.4)-.055,0.0,1.0);}
// The final sRGB fallback does not add an intermediate quantization. Keep the
// independently tested EOTF polynomial used by image_enhancement.frag.
#define DECODE(v) ((((((-.04895767106617191*(v)+.20727361103902037)*(v)-.4090387700149742)*(v)+.7008357119138372)*(v)+.5164904585720449)*(v)+.03244249830160861)*(v)+.0009453002069357841)
vec3 to_linear(vec3 v){
    if(GAMMA2_SHARPEN)return v*v;
    return clamp(mix(DECODE(v),v/12.92,lessThanEqual(v,vec3(.04045))),0.0,1.0);
}
vec2 encoded_pixel(ivec2 pixel){
    // Clamp in the complete E grid before applying the nonlinear D map.
    vec2 original=(vec2(clamp(pixel,ivec2(0),ivec2(params.viewport.zw)-1))+.5)*params.viewport.xy;
    return vec2(encoded_coordinate(original.x),encoded_coordinate(original.y));
}
vec3 composite_focus(vec3 base,vec2 pixel){
    if(params.options.z==0.0)return base;
    vec2 crop_uv=(pixel-params.focus_rect.xy)/params.focus_rect.zw;
    float alpha=clamp((0.5-length(crop_uv-vec2(.5)))*20.0,0.0,1.0);
    if(alpha==0.0)return base;
    return mix(base,sample_region(focus_eye,crop_uv,params.focus_source_rect),alpha);
}
// Backward mapping avoids holes inside the source FOV. Missing source rays
// are black, not smeared edge texels; no depth or scene-flow is invented.
vec3 timewarped_linear(vec2 at){
    vec4 f=params.timewarp_fov,q=params.timewarp_rotation;
    vec3 ray=vec3(mix(-f.x,f.y,at.x),mix(f.z,-f.w,at.y),-1.0);
    ray+=2.0*cross(q.xyz,cross(q.xyz,ray)+q.w*ray);
    if(ray.z>=-1e-6)return vec3(0);
    vec2 projected=ray.xy/-ray.z;
    vec2 source=vec2((projected.x+f.x)/(f.x+f.y),(f.z-projected.y)/(f.z+f.w));
    if(any(lessThan(source,vec2(0)))||any(greaterThan(source,vec2(1))))return vec3(0);
    vec2 encoded=vec2(encoded_coordinate(source.x),encoded_coordinate(source.y));
    return composite_focus(sample_eye(encoded),source*params.viewport.zw);
}
vec3 reconstructed_linear(ivec2 pixel){
    vec2 position=vec2(clamp(pixel,ivec2(0),ivec2(params.viewport.zw)-1))+.5;
    if(params.timewarp_fov.x>0.0)return timewarped_linear(position*params.viewport.xy);
    return composite_focus(sample_eye(encoded_pixel(pixel)),position);
}
vec3 reconstructed_code(ivec2 pixel){
    if(GAMMA2_SHARPEN)return sqrt(max(reconstructed_linear(pixel),vec3(0)));
    return round(to_srgb(reconstructed_linear(pixel))*255.0)/255.0;
}
float reconstructed_green(ivec2 pixel){
    if(GAMMA2_SHARPEN)return sqrt(max(reconstructed_linear(pixel).g,0.0));
    return round(to_srgb(reconstructed_linear(pixel).g)*255.0)/255.0;
}
float sharpen_delta(float center,vec4 neighbors){
    float lo=min(center,min(min(neighbors.x,neighbors.y),min(neighbors.z,neighbors.w)));
    float hi=max(center,max(max(neighbors.x,neighbors.y),max(neighbors.z,neighbors.w)));
    float amplitude=sqrt(clamp(min(lo,1.0-hi)/max(hi,1e-6),0.0,1.0));
    float weight=-.175*params.options.y*amplitude;
    float sharpened=(center+weight*dot(neighbors,vec4(1)))/(1.0+4.0*weight);
    return clamp(sharpened-center,-23.0/255.0,23.0/255.0);
}
