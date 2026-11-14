#include "foveation_gpu_test_cases.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"

namespace surfxr::tests::foveation_gpu {
namespace {
constexpr unsigned bw=32,bh=32,fw=24,fh=16,aw=bw*2,ah=bh+fh,width=128,height=96;
double encoded(double uv){return uv<.25?2*uv*uv:uv<.75?1.5*uv-.25:1-2*(1-uv)*(1-uv);}
unsigned code(unsigned eye,bool focus,int x,int y,unsigned channel,unsigned revision){
    x=std::clamp(x,0,int(focus?fw:bw)-1);y=std::clamp(y,0,int(focus?fh:bh)-1);
    if(focus)return 60+((unsigned(x+y+channel*3+eye*7+revision*5)%2)*80);
    const auto u=original_coordinate((x+.5)/bw),v=original_coordinate((y+.5)/bh);
    return unsigned(30+eye*30+revision*7+(channel==1?v:u)*70);
}
double yuv_linear(double y,unsigned eye,bool focus,unsigned channel){
    const double u=(double(focus?150:105)+eye*10-128)/255.,v=(double(focus?100:155)-eye*10-128)/255.;
    const std::array<double,3> rgb{{y+1.5748*v,y-.187324*u-.468124*v,y+1.8556*u}};
    const auto value=std::clamp(rgb[channel],0.,1.);
    return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);
}
double sample(unsigned eye,bool focus,double u,double v,unsigned c,bool nv12,unsigned revision){
    const double x=std::clamp(u*(focus?fw:bw)-.5,0.,double((focus?fw:bw)-1));
    const double y=std::clamp(v*(focus?fh:bh)-.5,0.,double((focus?fh:bh)-1));
    const int ix=int(x),iy=int(y);const auto fx=x-ix,fy=y-iy;
    double result=0;
    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){
        const auto byte=std::uint8_t(code(eye,focus,ix+dx,iy+dy,nv12?0:c,revision));
        result+=(dx?fx:1-fx)*(dy?fy:1-fy)*(nv12?byte/255.:decode_srgb(byte));
    }
    return nv12?yuv_linear(result,eye,focus,c):result;
}
void run_atlas(bool nv12,bool quad,bool timewarp){
    Vulkan vk;vk.initialize(true);
    for(auto& image:vk.targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.readback,width*height*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    vk.begin();for(auto& target:vk.targets)vk.barrier(target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    const auto rgba_allocator=surfxr::vulkan_rgba_frame_allocator(vk.physical,vk.device);
    const auto nv12_allocator=surfxr::vulkan_nv12_frame_allocator(vk.physical,vk.device);
    auto rgba=nv12?nullptr:rgba_allocator(aw*ah*4);
    auto planes=nv12?nv12_allocator(aw,ah):nullptr;
    auto* r=dynamic_cast<surfxr::VulkanRgbaFrameStorage*>(rgba.get());
    auto* n=dynamic_cast<surfxr::VulkanNv12FrameStorage*>(planes.get());
    require(nv12?(n&&valid_nv12_storage(*n)):(r&&r->prepare_sampled_image(vk.physical,aw,ah)),"atlas_storage_unavailable");
    surfxr::FoveationRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,format,width,height,quad);
    unsigned checked=0,maximum=0,revision=0;
    for(const auto offsets:std::array<std::array<unsigned,4>,3>{{{18,12,82,68},{76,34,28,58},{0,0,width-fw,height-fh}}}){
        if(nv12){for(unsigned p=0;p<2;++p)std::memset(n->data(p),0xcd,n->bytes[p]);}
        else std::memset(rgba->data(),0xcd,rgba->size());
        for(unsigned eye=0;eye<2;++eye)for(bool focus:{false,true}){
            const unsigned w=focus?fw:bw,h=focus?fh:bh,top=focus?bh:0;
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                if(nv12)n->data(0)[(y+top)*n->stride[0]+eye*bw+x]=std::uint8_t(code(eye,focus,x,y,0,revision));
                else {auto* out=rgba->data()+((y+top)*aw+eye*bw+x)*4;
                    for(unsigned c=0;c<3;++c)out[c]=std::uint8_t(code(eye,focus,x,y,c,revision));
                    out[3]=255;}
            }
            if(nv12)for(unsigned y=0;y<h/2;++y)for(unsigned x=0;x<w;x+=2){auto* uv=n->data(1)+(y+top/2)*n->stride[1]+eye*bw+x;
                uv[0]=std::uint8_t((focus?150:105)+eye*10);uv[1]=std::uint8_t((focus?100:155)-eye*10);}
        }
        std::array<surfxr::FoveationEye,2> eyes{};
        for(unsigned eye=0;eye<2;++eye){auto& f=eyes[eye];
            f={nv12?n->image(0):r->sampled_image(),nv12?n->view(0):r->sampled_view(),vk.targets[eye].view,true};
            f.host_source=f.focus_host_source=true;f.host_source_initialized=f.focus_host_initialized=revision!=0;
            f.source_rect={eye*.5f,0,.5f,float(bh)/ah};
            if(timewarp){const float angle=eye?-.13f:.17f;f.timewarp_rotation={0,std::sin(angle/2),0,std::cos(angle/2)};f.timewarp_fov={1,1,1,1};}
            f.focus_source=f.source;f.focus_view=f.source_view;
            f.focus_source_rect={eye*.5f,float(bh)/ah,float(fw)/aw,float(fh)/ah};
            f.focus_rect={float(offsets[eye*2]),float(offsets[eye*2+1]),fw,fh};
            if(nv12){f.chroma_source=n->image(1);f.chroma_view=n->view(1);f.yuv_coefficients={.2126f,.0722f,0,1};}
        }
        require(renderer.render(eyes,true),"atlas_render_skipped");
        if(nv12)n->mark_initialized();else r->mark_sampled_image_initialized();
        renderer.complete(false);const auto pixels=vk.read_pixels(width,height);
        for(unsigned eye=0;eye<2;++eye)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            double sx=x+.5,sy=y+.5;bool visible=true;
            if(timewarp){const double angle=eye?-.13:.17,rx=2*sx/width-1,ry=1-2*sy/height;
                const double z=-std::sin(angle)*rx-std::cos(angle),tx=std::cos(angle)*rx-std::sin(angle);
                sx=(tx/-z+1)*width/2;sy=(1-ry/-z)*height/2;
                visible=z<0&&sx>=0&&sx<=width&&sy>=0&&sy<=height;}
            const double u=(sx-offsets[eye*2])/fw,v=(sy-offsets[eye*2+1])/fh;
            const auto alpha=std::clamp((.5-std::hypot(u-.5,v-.5))*20,0.,1.);
            for(unsigned c=0;c<3;++c){
                const auto base=sample(eye,false,encoded(sx/width),encoded(sy/height),c,nv12,revision);
                const auto focus=sample(eye,true,u,v,c,nv12,revision);
                const auto expected=visible?encode_srgb(base*(1-alpha)+focus*alpha):0;
                const auto actual=pixels[(eye*width*height+y*width+x)*4+c];
                const auto error=unsigned(std::abs(int(actual)-int(expected)));
                require(error<=2,"atlas_nonlinear_focus_pixel_or_region_seam_error");maximum=std::max(maximum,error);++checked;
            }
            require(pixels[(eye*width*height+y*width+x)*4+3]==255,"atlas_alpha_error");
        }
        ++revision;
    }
    std::cout<<"{\"gaze_atlas\":\"passed\",\"nv12\":"<<(nv12?"true":"false")<<",\"pixel_channel_checks\":"<<checked
        <<",\"max_code_error\":"<<maximum<<",\"shared_atlas\":true,\"crop_movement\":true,\"poisoned_padding\":true,\"headset\":false}\n";
}
}
void run_gaze_atlas(bool timewarp){for(bool nv12:{false,true})for(bool quad:{false,true})run_atlas(nv12,quad,timewarp);}
}
