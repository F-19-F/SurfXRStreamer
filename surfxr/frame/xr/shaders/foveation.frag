#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef SURFXR_FOVEATION_QUAD
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_quad : require
#endif
layout(set=0,binding=0) uniform sampler2D encoded_eye;
layout(set=0,binding=1) uniform sampler2D focus_eye;
layout(set=0,binding=2) uniform sampler2D chroma_eye;
layout(constant_id=0) const bool SHARPEN=false;
layout(constant_id=1) const bool TARGET_ENCODED=false;
layout(constant_id=2) const bool ZERO_WEIGHT_SKIP=true;
layout(constant_id=3) const bool GAMMA2_SHARPEN=false;
layout(push_constant) uniform Parameters {vec4 viewport;vec4 options;vec4 focus_rect;vec4 source_rect;vec4 yuv;vec4 focus_source_rect;vec4 timewarp_rotation;vec4 timewarp_fov;} params;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
#include "nv12_sampling.glsl"
#include "foveation_postprocess.glsl"
void main(){
    if(SHARPEN){
        ivec2 pixel=ivec2(gl_FragCoord.xy);
        // Both algorithms use the same complete-eye sample geometry. The
        // legacy variant quantizes E8; Gamma2 keeps sqrt(linear) continuous.
        vec3 linear_base=GAMMA2_SHARPEN?reconstructed_linear(pixel):vec3(0);
        vec3 base=GAMMA2_SHARPEN?sqrt(max(linear_base,vec3(0))):reconstructed_code(pixel);
        bool zero_weight=base.g==0.0||base.g==1.0;
#ifdef SURFXR_FOVEATION_QUAD
        // Quad swaps include helper invocations at image borders. Each helper
        // reconstructs its clamped complete-E pixel, matching the edge rule.
        // Vulkan defines low subgroup lane bits as the quad's X/Y indices;
        // do not assume the quad origin is an even framebuffer coordinate.
        uint lane=gl_SubgroupInvocationID&3u;
        float horizontal=subgroupQuadSwapHorizontal(base.g);
        float vertical=subgroupQuadSwapVertical(base.g);
        zero_weight=zero_weight||horizontal==0.0||horizontal==1.0||vertical==0.0||vertical==1.0;
#endif
        // Both working domains lie in [0,1]. A known zero makes lo=0;
        // a known one makes hi=1. Either forces the sharpening amplitude to
        // exactly zero, independently of the remaining neighbors. Both quad
        // swaps above must execute before this varying branch, including in
        // helper invocations; no subgroup operation follows the early return.
        if(ZERO_WEIGHT_SKIP&&zero_weight){
            // Preserve the sampled linear value when the filter is inactive;
            // sqrt followed by squaring can otherwise change one output code.
            color=vec4(GAMMA2_SHARPEN?linear_base:(TARGET_ENCODED?base:to_linear(base)),1);
            return;
        }
#ifdef SURFXR_FOVEATION_QUAD
        float outside_x=reconstructed_green(pixel+ivec2((lane&1u)==0u?-1:1,0));
        float outside_y=reconstructed_green(pixel+ivec2(0,(lane&2u)==0u?-1:1));
        // Preserve the N/S/W/E arithmetic order of the portable five-tap path.
        vec4 neighbors=vec4((lane&2u)==0u?outside_y:vertical,(lane&2u)==0u?vertical:outside_y,
                            (lane&1u)==0u?outside_x:horizontal,(lane&1u)==0u?horizontal:outside_x);
#else
        vec4 neighbors=vec4(reconstructed_green(pixel+ivec2(0,-1)),reconstructed_green(pixel+ivec2(0,1)),
                            reconstructed_green(pixel+ivec2(-1,0)),reconstructed_green(pixel+ivec2(1,0)));
#endif
        float delta=GAMMA2_SHARPEN?gamma2_sharpen_delta(base,neighbors):sharpen_delta(base.g,neighbors);
        vec3 enhanced=clamp(base+delta,0.0,1.0);
        color=vec4(GAMMA2_SHARPEN&&delta==0.0?linear_base:(TARGET_ENCODED?enhanced:to_linear(enhanced)),1);
        return;
    }
    // Preserve the original no-enhancement path, with no software transfer or
    // quantization. Specialization removes all fused code from this pipeline.
    vec2 encoded=vec2(encoded_coordinate(uv.x),encoded_coordinate(uv.y));
    color=vec4(params.timewarp_fov.x>0.0?timewarped_linear(uv):composite_focus(sample_eye(encoded),gl_FragCoord.xy),1.0);
    if(params.options.x!=0.0){
        vec2 pixel=floor(gl_FragCoord.xy);
        float edge=min(min(pixel.x,pixel.y),min(params.viewport.z-1.0-pixel.x,params.viewport.w-1.0-pixel.y));
        color.a=edge<1.0?64.0/255.0:(edge<2.0?192.0/255.0:1.0);
    }
}
