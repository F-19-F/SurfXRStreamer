#version 450
// Fixed pipeline mode: baseline, stable quality, or owned mip0 conversion.
layout(constant_id=0) const uint render_mode=0u;
layout(location=0) in vec3 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec3 normal;
layout(push_constant) uniform Camera {mat4 vp;vec4 eye;vec4 sampling;} camera;
layout(location=0) out vec2 uv;
layout(location=1) out vec3 facing_normal;
layout(location=2) out vec3 to_eye;
void main(){
    if(render_mode>=2u){
        // Fullscreen triangle for the owned mip0. Source pixel centers are
        // fetched explicitly by the fragment shader; no XR image is retained.
        vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
        gl_Position=vec4(p*2.0-1.0,0.0,1.0);
        uv=p;facing_normal=vec3(0,0,1);to_eye=vec3(0,0,1);
        return;
    }
    gl_Position=camera.vp*vec4(position,1.0);
    uv=texcoord;facing_normal=normal;to_eye=camera.eye.xyz-position;
}
