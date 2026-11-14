#include "foveation_gpu_test_cases.hpp"

namespace surfxr::tests::foveation_gpu {
void run_focus(bool quad){
    constexpr unsigned sw=64,sh=64,width=192,height=160,fw=48,fh=32;
    Vulkan vk;vk.initialize(true);
    for(auto& image:vk.sources)vk.image(image,sw,sh,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.extra_targets)vk.image(image,fw,fh,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.upload,sw*sh*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,width*height*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const std::array<std::array<unsigned char,3>,2> base{{{{40,90,140}},{{70,110,160}}}},focus{{{{230,60,100}},{{50,210,80}}}};
    const auto focus_code=[&](unsigned e,int x,int y,unsigned c){
        x=std::clamp(x,0,int(fw)-1);y=std::clamp(y,0,int(fh)-1);
        // Pixel-scale edges detect any resize or half-pixel shift. Keep a
        // constant central patch for the fused-sharpening identity check.
        if(std::abs(x-int(fw/2))<4&&std::abs(y-int(fh/2))<4)return focus[e][c];
        return static_cast<unsigned char>((unsigned(focus[e][c])+unsigned((x+y)%2)*53u)%256u);
    };
    std::vector<std::uint8_t> bytes(sw*sh*8);
    for(unsigned e=0;e<2;++e)for(unsigned p=0;p<sw*sh;++p){
        for(unsigned c=0;c<3;++c)bytes[(e*sw*sh+p)*4+c]=base[e][c];
        bytes[(e*sw*sh+p)*4+3]=255;
    }
    vk.upload_pixels(bytes,sw,sh,true);
    bytes.resize(fw*fh*8);
    for(unsigned e=0;e<2;++e)for(unsigned p=0;p<fw*fh;++p){
        for(unsigned c=0;c<3;++c)bytes[(e*fw*fh+p)*4+c]=focus_code(e,p%fw,p/fw,c);
        bytes[(e*fw*fh+p)*4+3]=255;
    }
    void* mapped=nullptr;check(vkMapMemory(vk.device,vk.upload.memory,0,bytes.size(),0,&mapped),"focus_map");
    std::memcpy(mapped,bytes.data(),bytes.size());vkUnmapMemory(vk.device,vk.upload.memory);
    vk.begin();for(unsigned e=0;e<2;++e){
        const auto image=vk.extra_targets[e].image;
        vk.barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.bufferOffset=fw*fh*4*e;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={fw,fh,1};
        vkCmdCopyBufferToImage(vk.command,vk.upload.buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        vk.barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }vk.submit();
    surfxr::FoveationRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,format,width,height,quad,true,surfxr::FoveationSharpening::Gamma2);
    std::array<surfxr::FoveationEye,2> eyes{};
    for(unsigned e=0;e<2;++e){eyes[e]={vk.sources[e].image,vk.sources[e].view,vk.targets[e].view,true};
        eyes[e].focus_source=vk.extra_targets[e].image;eyes[e].focus_view=vk.extra_targets[e].view;
    }
    unsigned checked=0;
    // Noncentral, different L/R crops, a moved crop, then clipping at edges.
    for(const auto& offsets:std::array<std::array<float,4>,3>{{{{22,18,115,97}},{{93,40,45,75}},{{-10,60,165,-8}}}}){
        for(unsigned e=0;e<2;++e)eyes[e].focus_rect={offsets[e*2],offsets[e*2+1],fw,fh};
        require(renderer.render(eyes,true),"focus_render_skipped");const auto pixels=vk.read_pixels(width,height);renderer.complete(true);
        for(unsigned e=0;e<2;++e)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const double u=(x+.5-offsets[e*2])/fw-.5,v=(y+.5-offsets[e*2+1])/fh-.5;
            const double alpha=std::clamp((.5-std::hypot(u,v))*20,0.0,1.0);
            const auto p=(e*width*height+y*width+x)*4;require(pixels[p+3]==255,"focus_nonopaque");
            for(unsigned c=0;c<3;++c){
                const auto crop=focus_code(e,int(x)-int(offsets[e*2]),int(y)-int(offsets[e*2+1]),c);
                const auto expected=encode_srgb((1-alpha)*decode_srgb(base[e][c])+alpha*decode_srgb(crop));
                require(std::abs(int(pixels[p+c])-int(expected))<=1,"focus_position_mask_or_linear_blend");++checked;
            }
        }
        // Sharpening must sample the composite; its opaque constant center
        // remains the focus color, in portable and subgroup variants.
        require(renderer.render(eyes,false,.35f),"focus_sharpen_skipped");
        const auto sharpened=vk.read_pixels(width,height);
        for(unsigned e=0;e<2;++e){const int x=int(offsets[e*2]+fw*.5f),y=int(offsets[e*2+1]+fh*.5f);
            if(x>=0&&x<int(width)&&y>=0&&y<int(height))for(unsigned c=0;c<3;++c)
                require(std::abs(int(sharpened[(e*width*height+y*width+x)*4+c])-focus[e][c])<=1,"focus_fused_sharpen_lost_center");
        }
    }
    std::cout<<"{\"focus_composite\":\"passed\",\"checked_channels\":"<<checked<<",\"quad\":"<<renderer.supports_quad_sharpening()<<"}\n";
}
} // namespace surfxr::tests::foveation_gpu
