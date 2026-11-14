#include "frame/xr/generated/image_enhancement_reference.hpp"
#include "surfxr/postprocess_plan.hpp"
#include <iostream>
#include <limits>
#include <string>

namespace {
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
void tests(){
    const auto high=surfxr::image_enhancement_extent(2496,2688,true);
    require(high.width==2736&&high.height==2946&&high.upscale,"complete_eye_sizing_not_decoded_eye");
    const auto godlike=surfxr::image_enhancement_extent(3072,3264,true);
    require(godlike.width==3072&&godlike.height==3264&&!godlike.upscale&&godlike.scale==1,"never_downsample_high_quality");
    const auto disabled=surfxr::image_enhancement_extent(1344,1440,false);
    require(disabled.width==1344&&disabled.height==1440&&!disabled.upscale,"disabled_passthrough");
    bool rejected=false;try{surfxr::image_enhancement_extent(1,UINT32_MAX,true);}catch(...){rejected=true;}
    require(rejected&&!surfxr::valid_image_enhancement_options({true,std::numeric_limits<float>::quiet_NaN()}),"invalid_configuration");
    const auto plain_plan=surfxr::make_postprocess_plan(2048,2176,4608,4896,{false,0},8192,8192);
    require(plain_plan.decoded.width==2048&&plain_plan.complete.width==4608&&plain_plan.output.width==4608&&
            plain_plan.output.height==4896&&!plain_plan.enhancement_active&&!plain_plan.fused_sharpen&&plain_plan.pass_count==1&&
            plain_plan.variant==surfxr::PostprocessVariant::Bypass&&plain_plan.color_domain==surfxr::PostprocessColorDomain::SrgbCodeValues,"plan_disabled_preserves_complete_eye");
    const auto upscale_plan=surfxr::make_postprocess_plan(1024,1088,2496,2688,{true,0},8192,8192);
    require(upscale_plan.output.width==2736&&upscale_plan.output.height==2946&&upscale_plan.upscale_active&&
            upscale_plan.options.super_resolution&&!upscale_plan.fused_sharpen&&upscale_plan.pass_count==2&&
            upscale_plan.variant==surfxr::PostprocessVariant::Upscale&&upscale_plan.color_domain==surfxr::PostprocessColorDomain::SrgbCodeValues,"plan_uses_complete_not_decoded_extent");
    const auto fused_plan=surfxr::make_postprocess_plan(1024,1088,2496,2688,{true,.65f},8192,8192);
    require(fused_plan.pass_count==2&&!fused_plan.fused_sharpen&&fused_plan.options.sharpness==.65f&&
            fused_plan.color_domain==surfxr::PostprocessColorDomain::SrgbCodeValues&&
            fused_plan.point_stage==surfxr::PostprocessPointStage::AfterSpatialBeforeAttachmentTransfer&&
            fused_plan.variant==surfxr::PostprocessVariant::UpscaleSharpen,"plan_fuses_spatial_effects_in_code_domain");
    for(const auto limits:std::array<std::array<unsigned,2>,2>{{{{2600,8192}},{{8192,2800}}}}){
        const auto limited=surfxr::make_postprocess_plan(1024,1088,2496,2688,{true,.4f},limits[0],limits[1]);
        require(limited.output.width==2496&&limited.output.height==2688&&!limited.upscale_active&&
                !limited.options.super_resolution&&limited.options.sharpness==.4f&&limited.fused_sharpen&&limited.pass_count==1&&
                limited.variant==surfxr::PostprocessVariant::Sharpen&&limited.color_domain==surfxr::PostprocessColorDomain::Gamma2,"plan_limit_fallback_keeps_sharpening");
    }
    const auto godlike_plan=surfxr::make_postprocess_plan(2048,2176,4608,4896,{true,.3f},8192,8192);
    require(godlike_plan.output.width==4608&&godlike_plan.output.height==4896&&!godlike_plan.upscale_active&&
            godlike_plan.enhancement_active&&godlike_plan.fused_sharpen&&godlike_plan.pass_count==1&&
            godlike_plan.options.sharpness==.3f&&godlike_plan.color_domain==surfxr::PostprocessColorDomain::Gamma2,"plan_never_downsamples_godlike");
    unsigned invalid_plans=0;
    for(unsigned scenario=0;scenario<5;++scenario){
        try{surfxr::make_postprocess_plan(scenario==0?0:32,24,48,36,
                                    {true,scenario==1?std::numeric_limits<float>::quiet_NaN():scenario==2?-1.f:.5f},
                                    scenario==3?0:8192,scenario==4?35:8192);}
        catch(const std::invalid_argument&){++invalid_plans;}
    }
    require(invalid_plans==5,"plan_rejects_invalid_dimensions_options_limits");
    using namespace surfxr::enhancement_reference;
    for(unsigned i=0;i<=262144;++i){const float x=float(i)/262144;
        require(std::abs(fast_srgb(x)-srgb(x))<=.00042f,"fast_oetf_exceeds_color_error_bound");
        require(std::abs(fast_linear(x)-linear(x))<=.00002f,"fast_eotf_exceeds_color_error_bound");
    }
    Image image{7,5,std::vector<std::uint8_t>(7*5*4)};
    for(unsigned i=0;i<35;++i){image.rgba[i*4]=25;image.rgba[i*4+1]=128;image.rgba[i*4+2]=220;image.rgba[i*4+3]=255;}
    for(bool sr:{false,true})for(float sharp:{0.f,.35f,1.f})for(unsigned y=0;y<11;++y)for(unsigned x=0;x<17;++x){
        const auto color=enhance(image,(x+.5f)/17,(y+.5f)/11,{sr,sharp});
        require(std::abs(color[0]-25.f/255)<2e-6&&std::abs(color[1]-128.f/255)<2e-6&&std::abs(color[2]-220.f/255)<2e-6,"constant_color_gamma_and_edges");
    }
    image=fixture(32,24,0);unsigned sr_changed=0,sharp_changed=0;
    for(unsigned y=0;y<36;++y)for(unsigned x=0;x<48;++x){
        const float u=(x+.5f)/48,v=(y+.5f)/36;const auto plain=enhance(image,u,v,{});
        const auto sr=enhance(image,u,v,{true,0}),sharp=enhance(image,u,v,{false,1}),both=enhance(image,u,v,{true,1});
        sr_changed+=std::abs(sr[1]-plain[1])>1e-4;sharp_changed+=std::abs(sharp[1]-plain[1])>1e-4;
        for(auto color:{plain,sr,sharp,both})for(float value:color)require(std::isfinite(value)&&value>=0&&value<=1,"finite_clamped_output");
    }
    require(sr_changed>100&&sharp_changed>100,"independent_sr_and_sharpen_controls");
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){
        const auto color=enhance(image,(x+.5f)/image.width,(y+.5f)/image.height,{});
        for(unsigned c=0;c<3;++c)require(std::abs(color[c]*255-image.rgba[(y*image.width+x)*4+c])<.002,"one_to_one_no_effect_identity");
    }
}
}
int main(int argc,char** argv){
    if(argc!=2||std::string(argv[1])!="--self-test")return 1;
    try{tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":14,\"gpu_used\":false,\"network_used\":false,\"openxr_session_started\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
