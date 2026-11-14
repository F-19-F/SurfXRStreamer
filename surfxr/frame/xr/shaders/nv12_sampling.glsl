// Shared SDR NV12 sampling; crop edges are clamped independently per plane.
vec3 sample_nv12_code(sampler2D image,sampler2D chroma,vec2 eye_uv,vec4 rect,vec4 coefficients){
    vec2 edge=.5/vec2(textureSize(image,0));
    vec2 at=clamp(rect.xy+eye_uv*rect.zw,rect.xy+edge,rect.xy+rect.zw-edge);
    float y=(textureLod(image,at,0).r-coefficients.z)*coefficients.w;
    vec2 uv_edge=.5/vec2(textureSize(chroma,0));
    vec2 uv_at=clamp(rect.xy+eye_uv*rect.zw,rect.xy+uv_edge,rect.xy+rect.zw-uv_edge);
    vec2 cbcr=(textureLod(chroma,uv_at,0).rg-vec2(128.0/255.0))*(coefficients.z>0.0?255.0/224.0:1.0);
    float kr=coefficients.x,kb=coefficients.y,kg=1.0-kr-kb;
    vec3 code=clamp(vec3(y+2.0*(1.0-kr)*cbcr.y,
        y-2.0*kb*(1.0-kb)/kg*cbcr.x-2.0*kr*(1.0-kr)/kg*cbcr.y,
        y+2.0*(1.0-kb)*cbcr.x),0.0,1.0);
    return code;
}
vec3 nv12_linear_from_code(vec3 code){
    return mix(pow((code+.055)/1.055,vec3(2.4)),code/12.92,lessThanEqual(code,vec3(.04045)));
}
