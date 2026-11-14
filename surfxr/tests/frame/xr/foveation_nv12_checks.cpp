#include "foveation_gpu_test_cases.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"

namespace surfxr::tests::foveation_gpu {
void run_nv12(bool packed){
    Vulkan vk;vk.initialize(true);
    const auto allocator=surfxr::vulkan_nv12_frame_allocator(vk.physical,vk.device);
    if(!allocator){std::cout<<"{\"nv12_source_checked\":false,\"supported\":false}\n";return;}
    std::array<std::shared_ptr<surfxr::Nv12FrameStorage>,2> owners;owners[0]=allocator(packed?16:8,8);
    owners[1]=packed?owners[0]:allocator(8,8);
    std::array<surfxr::VulkanNv12FrameStorage*,2> sources{};
    for(unsigned eye=0;eye<2;++eye){sources[eye]=dynamic_cast<surfxr::VulkanNv12FrameStorage*>(owners[eye].get());
        require(sources[eye]&&valid_nv12_storage(*sources[eye]),"nv12_gpu_allocation_failed");}
    constexpr unsigned width=96,height=64;
    for(auto& image:vk.targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.readback,width*height*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    vk.begin();for(auto& target:vk.targets)vk.barrier(target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    surfxr::FoveationRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,format,width,height);
    unsigned checks=0,max_error=0;
    // Independent BT.601/709/2020 NCL YCbCr reference coefficients. Compare
    // encoded output bytes, including range endpoints and both sides of the
    // packed-eye seam. Padding contains poison, never visible source pixels.
    const std::array<std::array<double,4>,3> matrices{{{1.402,.344136,.714136,1.772},
        {1.5748,.187324,.468124,1.8556},{1.4746,.164553,.571353,1.8814}}};
    const std::array<std::array<float,2>,3> weights{{{.299f,.114f},{.2126f,.0722f},{.2627f,.0593f}}};
    for(unsigned matrix=0;matrix<3;++matrix)for(bool limited:{false,true})for(unsigned pattern=0;pattern<2;++pattern){
        const std::array<std::array<unsigned,3>,2> codes=pattern?
            std::array<std::array<unsigned,3>,2>{{{limited?16u:0u,128,128},{limited?235u:255u,128,128}}}:
            std::array<std::array<unsigned,3>,2>{{{102,90,240},{153,200,30}}};
        for(unsigned p=0;p<2;++p){for(unsigned eye=0;eye<(packed?1u:2u);++eye)std::memset(sources[eye]->data(p),0xcd,sources[eye]->bytes[p]);
            for(unsigned row=0;row<(p?4u:8u);++row)for(unsigned eye=0;eye<2;++eye)for(unsigned x=0;x<8;++x)
                sources[eye]->data(p)[row*sources[eye]->stride[p]+(packed?eye*8:0)+x]=std::uint8_t(codes[eye][p?(x%2?2:1):0]);}
        std::array<surfxr::FoveationEye,2> eyes{};
        for(unsigned eye=0;eye<2;++eye){auto& source=*sources[eye];auto& f=eyes[eye];f={source.image(0),source.view(0),vk.targets[eye].view,true};
            f.host_source=true;f.host_source_initialized=source.initialized();f.linear_layout=packed;
            f.source_rect=packed?std::array<float,4>{eye*.5f,0,.5f,1}:std::array<float,4>{0,0,1,1};
            f.chroma_source=source.image(1);f.chroma_view=source.view(1);
            f.desktop_border=packed&&eye==1;
            f.yuv_coefficients={weights[matrix][0],weights[matrix][1],limited?16.f/255.f:0.f,limited?255.f/219.f:1.f};}
        require(renderer.render(eyes,true),"nv12_render_skipped");for(auto* source:sources)source->mark_initialized();renderer.complete(false);
        const auto result=vk.read_pixels(width,height);
        for(unsigned eye=0;eye<2;++eye){const auto& c=codes[eye];const auto& m=matrices[matrix];
            const double y=(double(c[0])-(limited?16:0))/(limited?219:255),u=(double(c[1])-128)/(limited?224:255),v=(double(c[2])-128)/(limited?224:255);
            const std::array<double,3> reference{{y+m[0]*v,y-m[1]*u-m[2]*v,y+m[3]*u}};
            for(unsigned pixel=0;pixel<width*height;++pixel)for(unsigned channel=0;channel<4;++channel){
                const unsigned x=pixel%width,y=pixel/width,edge=std::min({x,y,width-1-x,height-1-y});
                const unsigned alpha=packed&&eye==1&&edge<2?(edge?192:64):255;
                const unsigned expected=channel==3?alpha:unsigned(std::lround(std::clamp(reference[channel],0.,1.)*255));
                const auto actual=result[(eye*width*height+pixel)*4+channel];const auto error=unsigned(std::abs(int(actual)-int(expected)));
                require(error<=2,"nv12_matrix_range_eye_seam_or_padding_error");max_error=std::max(max_error,error);++checks;
            }
        }
    }
    std::cout<<"{\"nv12_source_checked\":true,\"matrix_range_cases\":12,\"pixel_channel_checks\":"<<checks
        <<",\"max_code_error\":"<<max_error<<",\"packed_eye_seams\":"<<(packed?"true":"false")
        <<",\"surfxr_independent_nonlinear_eyes\":"<<(packed?"false":"true")<<",\"poisoned_padding\":true,\"pooled_general_rewrite\":true,\"openxr_session_started\":false}\n";
}
}
