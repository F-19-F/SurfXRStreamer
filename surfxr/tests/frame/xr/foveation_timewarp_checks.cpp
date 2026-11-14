#include "foveation_gpu_test_cases.hpp"
#include "frame/xr/rotational_timewarp.hpp"

namespace surfxr::tests::foveation_gpu {
void run_timewarp(){
    constexpr unsigned size=256;
    unsigned checked=0,black=0;double maximum=0;
    for(bool linear:{false,true})for(bool quad:{false,true}){
        Vulkan vk;vk.initialize(true);
        for(auto& image:vk.sources)vk.image(image,size,size,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        for(auto& image:vk.targets)vk.image(image,size,size,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        vk.buffer(vk.upload,size*size*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,size*size*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        FoveationRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,format,size,size,quad,true,FoveationSharpening::Gamma2);
        std::array<FoveationEye,2> eyes{};
        for(unsigned e=0;e<2;++e){eyes[e]={vk.sources[e].image,vk.sources[e].view,vk.targets[e].view,true};eyes[e].linear_layout=linear;}
        vk.upload_pixels(encoded_fixture(size,size,false,linear),size,size,true);
        require(renderer.render(eyes),"timewarp_baseline_skipped");const auto baseline=vk.read_pixels(size,size);
        for(unsigned axis=0;axis<3;++axis)for(double angle:{0.,.21,-.17,3.141592653589793}){
            for(unsigned e=0;e<2;++e){
                std::array<float,4> q{0,0,0,float(std::cos(angle/2))};q[axis]=float(std::sin(angle/2));
                xr_detail::RotationalTimewarp warp;
                require(xr_detail::rotational_timewarp({0,0,0,1},q,e?std::array<float,4>{.8f,1.1f,.9f,1.2f}:std::array<float,4>{1.1f,.8f,1.2f,.9f},warp),"timewarp_parameters_rejected");
                eyes[e].timewarp_rotation=warp.rotation;eyes[e].timewarp_fov=warp.fov;
            }
            std::vector<std::uint8_t> first;
            for(float sharpness:{0.f,.35f,0.f}){
                require(renderer.render(eyes,false,sharpness),"timewarp_render_skipped");const auto pixels=vk.read_pixels(size,size);
                if(sharpness==0){if(first.empty())first=pixels;else require(first==pixels,"timewarp_accumulated_or_source_mutated");}
                for(unsigned e=0;e<2;++e)for(unsigned y=3;y<size;y+=11)for(unsigned x=3;x<size;x+=11){
                    const auto& f=eyes[e].timewarp_fov;
                    const double u=(x+.5)/size,v=(y+.5)/size;
                    std::array<double,3> ray{-f[0]+u*(f[0]+f[1]),f[2]-v*(f[2]+f[3]),-1};
                    // Independent axis rotation matrix, not the shader's
                    // quaternion cross-product implementation.
                    const unsigned a=(axis+1)%3,b=(axis+2)%3;
                    const double ra=ray[a],rb=ray[b];
                    ray[a]=std::cos(angle)*ra-std::sin(angle)*rb;
                    ray[b]=std::sin(angle)*ra+std::cos(angle)*rb;
                    const double su=(ray[0]/-ray[2]+f[0])/(f[0]+f[1]),sv=(f[2]-ray[1]/-ray[2])/(f[2]+f[3]);
                    const auto actual=pixel_at(pixels,size,size,e,u,v);
                    if(ray[2]>=0||su<-.02||su>1.02||sv<-.02||sv>1.02){
                        require(actual[0]==0&&actual[1]==0&&actual[2]==0,"timewarp_missing_ray_not_black");++black;continue;
                    }
                    if(su<.04||su>.96||sv<.04||sv>.96)continue;
                    const auto error=std::max({std::abs(actual[0]-su),std::abs(actual[1]-sv),std::abs(actual[2]-(e?.72:.18))});
                    require(error<.012,"timewarp_rotation_fov_eye_or_sharpen_error");maximum=std::max(maximum,error);++checked;
                }
            }
        }
        for(auto& eye:eyes){eye.timewarp_rotation={0,0,0,1};eye.timewarp_fov={};}
        require(renderer.render(eyes),"timewarp_restore_skipped");
        require(vk.read_pixels(size,size)==baseline,"timewarp_disabled_changed_source_reconstruction");
    }
    std::cout<<"{\"rotational_timewarp\":\"passed\",\"sample_checks\":"<<checked<<",\"black_ray_checks\":"<<black
        <<",\"max_linear_error\":"<<maximum<<",\"headset_rendering\":false}\n";
    run_gaze_atlas(true);
}
}
