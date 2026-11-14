#include "projection_renderer_test_cases.hpp"
namespace surfxr::tests::projection {
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}

void checked(VkResult result,const char* label){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(label)+"_"+std::to_string(result));}

bool near(float a,float b,float tolerance){return std::abs(a-b)<tolerance;}

std::array<float,4> transform(const std::array<float,16>& m,surfxr::UiVec3 p){
    std::array<float,4> result{};const float v[]={p.x,p.y,p.z,1};
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)result[row]+=m[col*4+row]*v[col];
    return result;
}

surfxr::UiVec3 ndc(const std::array<float,16>& m,surfxr::UiVec3 p){const auto q=transform(m,p);require(q[3]>0,"clip_positive_w");return {q[0]/q[3],q[1]/q[3],q[2]/q[3]};}

unsigned cpu_tests(){
    unsigned groups=0;const std::array<float,4> fov{{-.6f,.8f,.7f,-.5f}};
    const auto matrix=surfxr::projection_view_matrix({},fov,.05f,100.f);
    require(near(ndc(matrix,{2*std::tan(fov[0]),0,-2}).x,-1)&&near(ndc(matrix,{2*std::tan(fov[1]),0,-2}).x,1),"asymmetric_horizontal_frustum");++groups;
    require(near(ndc(matrix,{0,2*std::tan(fov[2]),-2}).y,-1)&&near(ndc(matrix,{0,2*std::tan(fov[3]),-2}).y,1),"vulkan_y_up_maps_top");++groups;
    require(near(ndc(matrix,{0,0,-.05f}).z,0)&&near(ndc(matrix,{0,0,-100}).z,1),"vulkan_depth_zero_to_one");
    const auto middle=ndc(matrix,{std::tan(fov[0])+std::tan(fov[1]),std::tan(fov[2])+std::tan(fov[3]),-2});require(near(middle.x,0)&&near(middle.y,0),"asymmetric_frustum_center");++groups;
    const surfxr::UiPose eye{{1.3f,-.2f,4.f},{0,std::sin(.3f),0,std::cos(.3f)}};
    const auto moved=surfxr::projection_view_matrix(eye,fov,.05f,100.f);const surfxr::UiVec3 local{.3f,.4f,-2};
    const auto world=surfxr::ui_compose(eye,{local,{}}).position;const auto a=ndc(matrix,local),b=ndc(moved,world);
    require(near(a.x,b.x)&&near(a.y,b.y)&&near(a.z,b.z),"inverse_eye_pose");++groups;
    surfxr::UiPanel panel;panel.pose={{.5f,.25f,-3},{0,std::sin(.2f),0,std::cos(.2f)}};panel.width=8;panel.height=4.5f;
    for(float curvature:{0.f,1.8f}){
        panel.curvature_radians=curvature;const auto mesh=surfxr::projection_panel_mesh(panel,64);
        require(mesh.size()==64*6,"mesh_64_segments_triangle_count");
        for(const auto& v:mesh){
            require(v.uv[0]>=0&&v.uv[0]<=1&&v.uv[1]>=0&&v.uv[1]<=1,"mesh_uv_range");
            const auto expected=surfxr::ui_panel_point(panel,v.uv[0],v.uv[1]);
            require(near(v.position[0],expected.x)&&near(v.position[1],expected.y)&&near(v.position[2],expected.z),"mesh_matches_eight_meter_arc");
            const float theta=(v.uv[0]-.5f)*curvature;const auto normal=surfxr::ui_rotate(panel.pose.orientation,{-std::sin(theta),0,std::cos(theta)});
            require(near(v.normal[0],normal.x)&&near(v.normal[1],normal.y)&&near(v.normal[2],normal.z),"mesh_inward_front_normals");
        }
        for(std::size_t i=0;i<mesh.size();i+=3){const auto& p=mesh[i];const auto& q=mesh[i+1];const auto& r=mesh[i+2];
            const float ux=q.position[0]-p.position[0],uy=q.position[1]-p.position[1],uz=q.position[2]-p.position[2];
            const float vx=r.position[0]-p.position[0],vy=r.position[1]-p.position[1],vz=r.position[2]-p.position[2];
            require((uy*vz-uz*vy)*p.normal[0]+(uz*vx-ux*vz)*p.normal[1]+(ux*vy-uy*vx)*p.normal[2]>0,"mesh_front_triangle_winding");
        }
        ++groups;
    }
    return groups;
}

bool quadrant_color(const std::uint8_t* p,unsigned quadrant){
    if(p[3]<250)return false;
    if(quadrant==0)return p[0]>150&&p[0]>p[1]+60&&p[0]>p[2]+60;
    if(quadrant==1)return p[1]>150&&p[1]>p[0]+60&&p[1]>p[2]+60;
    if(quadrant==2)return p[2]>150&&p[2]>p[0]+60&&p[2]>p[1]+60;
    return p[0]>150&&p[1]>150&&p[2]<100;
}

void gpu_preview(const char* path,unsigned target,unsigned max_samples,surfxr::ProjectionSampling sampling){
    const auto groups=cpu_tests();NewFile output(path);Vulkan vk;vk.initialize();
    vk.image(vk.source,kSourceWidth,kSourceHeight,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,target,target,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    constexpr std::size_t source_bytes=kSourceWidth*kSourceHeight*4;
    const std::size_t eye_bytes=std::size_t{target}*target*4;
    vk.buffer(vk.upload,source_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,eye_bytes*2,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<std::uint8_t> pixels(source_bytes);const std::uint8_t colors[4][4]={{230,35,25,255},{20,230,45,255},{35,50,230,255},{230,220,25,255}};
    for(unsigned y=0;y<kSourceHeight;++y)for(unsigned x=0;x<kSourceWidth;++x){const auto quadrant=(y>=kSourceHeight/2?2:0)+(x>=kSourceWidth/2?1:0);auto* p=pixels.data()+(y*kSourceWidth+x)*4;std::memcpy(p,colors[quadrant],4);if(x%32==0||y%32==0)p[0]=p[1]=p[2]=245;}
    void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.upload.memory,0,source_bytes,0,&mapped),"vkMapMemory_upload");std::memcpy(mapped,pixels.data(),source_bytes);vkUnmapMemory(vk.device,vk.upload.memory);
    vk.begin();vk.barrier(vk.source.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={kSourceWidth,kSourceHeight,1};vkCmdCopyBufferToImage(vk.command,vk.upload.buffer,vk.source.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    vk.barrier(vk.source.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    for(auto& target:vk.targets)vk.barrier(target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vk.submit();surfxr::UiPanel panel;panel.pose.position.z=-2;panel.width=2;panel.height=1;panel.curvature_radians=1.2f;
    std::array<surfxr::ProjectionEye,2> eyes{};for(unsigned i=0;i<2;++i){eyes[i].target=vk.targets[i].view;eyes[i].pose.position.x=i==0?-.032f:.032f;eyes[i].fov={-.7853982f,.7853982f,.7853982f,-.7853982f};}
    constexpr unsigned warmup_frames=20,measured_frames=50;
    unsigned samples=0;double wait_ms=0,wait_max_ms=0;
    {surfxr::ProjectionRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,max_samples);
        const std::vector<surfxr::ProjectionSource> sources{{vk.source.image,vk.source.view,panel,kSourceWidth,kSourceHeight,0,1}};
        samples=renderer.samples();require((samples==1||samples==2||samples==4)&&samples<=max_samples,"renderer_respects_sample_cap");
        for(unsigned i=0;i<warmup_frames+measured_frames;++i){renderer.render(sources,eyes,sampling,i);if(i>=warmup_frames){const auto elapsed=renderer.last_gpu_wait_ms();require(std::isfinite(elapsed)&&elapsed>=0,"finite_gpu_wait_time");wait_ms+=elapsed;wait_max_ms=std::max(wait_max_ms,elapsed);}}
        if(sampling==surfxr::ProjectionSampling::QualitySS){const auto stats=renderer.statistics();
            require(stats.mip_updates==1&&stats.mip_reuses==warmup_frames+measured_frames-1&&stats.cached_levels==10,"quality_preview_cached_complete_mips");}
        wait_ms/=measured_frames;
    }
    vk.begin();for(unsigned i=0;i<2;++i){vk.barrier(vk.targets[i].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};region.bufferOffset=eye_bytes*i;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={target,target,1};vkCmdCopyImageToBuffer(vk.command,vk.targets[i].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,vk.readback.buffer,1,&region);
    }
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=vk.readback.buffer;host.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(vk.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);vk.submit();
    pixels.resize(eye_bytes*2);checked(vkMapMemory(vk.device,vk.readback.memory,0,pixels.size(),0,&mapped),"vkMapMemory_readback");std::memcpy(pixels.data(),mapped,pixels.size());vkUnmapMemory(vk.device,vk.readback.memory);
    std::array<unsigned,2> opaque{},transparent{},partial{};
    for(unsigned eye=0;eye<2;++eye){const auto* data=pixels.data()+eye*eye_bytes;
        for(unsigned i=0;i<target*target;++i){const auto alpha=data[i*4+3];if(alpha==0)++transparent[eye];else if(alpha==255)++opaque[eye];else ++partial[eye];}
        const auto minimum_area=std::uint64_t{target}*target*10000/(512*512);
        require(opaque[eye]>minimum_area&&transparent[eye]>minimum_area,"nonempty_opaque_panel_and_transparent_background");require(partial[eye]>0,"fractional_edge_alpha");
        for(const auto position:std::array<std::array<unsigned,2>,4>{{{{0,0}},{{target-1,0}},{{0,target-1}},{{target-1,target-1}}}})require(data[(position[1]*target+position[0])*4+3]==0,"transparent_background_corners");
        const auto m=surfxr::projection_view_matrix(eyes[eye].pose,eyes[eye].fov);
        for(unsigned quadrant=0;quadrant<4;++quadrant){const float u=(quadrant&1)?.73f:.23f,v=(quadrant&2)?.77f:.27f;const auto p=ndc(m,surfxr::ui_panel_point(panel,u,v));const int x=static_cast<int>((p.x+1)*target/2),y=static_cast<int>((p.y+1)*target/2);unsigned matches=0;const int radius=std::max(2u,(target+128)/256);
            require(x>radius&&x<static_cast<int>(target)-radius-1&&y>radius&&y<static_cast<int>(target)-radius-1,"quadrant_inside_target");for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx)if(quadrant_color(data+((y+dy)*target+x+dx)*4,quadrant))++matches;
            require(matches*5>=static_cast<unsigned>((2*radius+1)*(2*radius+1))*3,"quadrant_color_uv_and_vulkan_y");
        }
    }
    const std::string header="P6\n"+std::to_string(target*2)+" "+std::to_string(target)+"\n255\n";output.write(header.data(),header.size());std::vector<std::uint8_t> row(target*2*3);
    for(unsigned y=0;y<target;++y){for(unsigned eye=0;eye<2;++eye)for(unsigned x=0;x<target;++x){const auto* p=pixels.data()+eye*eye_bytes+(y*target+x)*4;for(unsigned c=0;c<3;++c)row[(eye*target+x)*3+c]=p[c];}output.write(row.data(),row.size());}output.finish();
    std::cout<<"{\"status\":\"gpu_preview_passed\",\"quality_ss\":"<<(sampling==surfxr::ProjectionSampling::QualitySS?"true":"false")<<",\"cpu_scenario_groups\":"<<groups<<",\"eye_width\":"<<target<<",\"eye_height\":"<<target<<",\"samples\":"<<samples<<",\"sample_cap\":"<<max_samples<<",\"warmup_frames\":"<<warmup_frames<<",\"measured_frames\":"<<measured_frames<<",\"gpu_wait_ms\":"<<wait_ms<<",\"gpu_wait_ms_mean\":"<<wait_ms<<",\"gpu_wait_ms_max\":"<<wait_max_ms<<",\"opaque_pixels\":["<<opaque[0]<<','<<opaque[1]<<"],\"transparent_pixels\":["<<transparent[0]<<','<<transparent[1]<<"],\"fractional_alpha_pixels\":["<<partial[0]<<','<<partial[1]<<"],\"quadrants_verified\":8,\"network_used\":false,\"openxr_session_started\":false}\n";
}

double linear_srgb(unsigned value){const double v=value/255.0;return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);}

unsigned encoded_srgb(double value){value=std::clamp(value,0.0,1.0);return static_cast<unsigned>(std::lround(255*(value<=.0031308?value*12.92:1.055*std::pow(value,1/2.4)-.055)));}

std::array<unsigned,4> reference_small_quality(const std::vector<std::uint8_t>& data,double u,double v){
    const auto sample=[&](double x,double y){
        x=x*kSourceWidth-.5;y=y*kSourceHeight-.5;const int left=static_cast<int>(std::floor(x)),top=static_cast<int>(std::floor(y));
        const double fx=x-left,fy=y-top;std::array<double,4> color{};
        for(int j=0;j<2;++j)for(int i=0;i<2;++i){const auto px=std::clamp(left+i,0,static_cast<int>(kSourceWidth)-1),py=std::clamp(top+j,0,static_cast<int>(kSourceHeight)-1);
            const auto* p=data.data()+(py*kSourceWidth+px)*4;const double weight=(i?fx:1-fx)*(j?fy:1-fy),alpha=p[3]/255.0;
            for(unsigned c=0;c<3;++c)color[c]+=weight*linear_srgb(p[c])*alpha;
            color[3]+=weight*alpha;
        }return color;
    };
    const auto result=sample(u,v);
    return {encoded_srgb(result[0]),encoded_srgb(result[1]),encoded_srgb(result[2]),static_cast<unsigned>(std::lround(std::clamp(result[3],0.0,1.0)*255))};
}

std::vector<ReferenceMip> reference_mips(const std::vector<std::uint8_t>& rgba,unsigned width,unsigned height){
    std::vector<ReferenceMip> result{{width,height,{}}};result[0].linear.resize(std::size_t{width}*height);
    for(std::size_t i=0;i<result[0].linear.size();++i)result[0].linear[i]=linear_srgb(rgba[i*4+1]);
    // Fixtures are opaque grayscale and power-of-two sized. Quantize every
    // mip to sRGB8, matching storage while keeping all sampling math in double.
    while(result.back().width>1||result.back().height>1){
        const auto& prev=result.back();ReferenceMip next{std::max(1u,prev.width/2),std::max(1u,prev.height/2),{}};
        next.linear.resize(std::size_t{next.width}*next.height);
        for(unsigned y=0;y<next.height;++y)for(unsigned x=0;x<next.width;++x){
            double value=0;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)
                value+=prev.linear[std::min(y*2+dy,prev.height-1)*prev.width+std::min(x*2+dx,prev.width-1)]*.25;
            next.linear[y*next.width+x]=linear_srgb(encoded_srgb(value));
        }
        result.push_back(std::move(next));
    }
    return result;
}

double reference_sample(const std::vector<ReferenceMip>& mips,double u,double v,double lod){
    const auto sample=[&](const ReferenceMip& mip){
        const double x=u*mip.width-.5,y=v*mip.height-.5;
        const int left=static_cast<int>(std::floor(x)),top=static_cast<int>(std::floor(y));const double fx=x-left,fy=y-top;
        double result=0;for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
            const auto ix=std::clamp(left+dx,0,static_cast<int>(mip.width)-1),iy=std::clamp(top+dy,0,static_cast<int>(mip.height)-1);
            result+=mip.linear[iy*mip.width+ix]*(dx?fx:1-fx)*(dy?fy:1-fy);
        }return result;
    };
    lod=std::clamp(lod,0.,double(mips.size()-1));const auto lo=static_cast<unsigned>(lod),hi=std::min(lo+1,static_cast<unsigned>(mips.size()-1));
    const double t=lod-lo;return sample(mips[lo])*(1-t)+sample(mips[hi])*t;
}

double legacy_quality_sample(const std::vector<ReferenceMip>& mips,double u,double v,double rho,unsigned phase){
    const double t=std::clamp(rho/1.414*std::sqrt(2.)-2.,0.,1.),a=.30-.15*t,b=1.25-.75*t;
    const double wi=.60-.30*t,wo=-.10+.30*t,du=rho/mips[0].width,dv=rho/mips[0].height;
    const double direction=phase?-1:1,lod=std::log2(rho)-1;
    const auto tap=[&](double offset){return reference_sample(mips,u+offset*direction*du,v+offset*dv,lod);};
    return std::clamp(wi*(tap(-a)+tap(a))+wo*(tap(-b)+tap(b)),0.,1.);
}

double reference_box(const ReferenceMip& source,double u,double v,double rho){
    const double left=u*source.width-rho*.5,right=left+rho,top=v*source.height-rho*.5,bottom=top+rho;
    double result=0;for(int y=static_cast<int>(std::floor(top));y<static_cast<int>(std::ceil(bottom));++y)
        for(int x=static_cast<int>(std::floor(left));x<static_cast<int>(std::ceil(right));++x){
            const double weight=(std::min(right,double(x+1))-std::max(left,double(x)))*
                                (std::min(bottom,double(y+1))-std::max(top,double(y)));
            const auto ix=std::clamp(x,0,static_cast<int>(source.width)-1),iy=std::clamp(y,0,static_cast<int>(source.height)-1);
            result+=weight*source.linear[iy*source.width+ix];
        }
    return result/(rho*rho);
}

void grayscale_fixture(std::vector<std::uint8_t>& rgba,unsigned width,unsigned height,unsigned pattern){
    // One-pixel strokes, a diagonal fine grid, and repeated 5x7 E/H glyphs.
    // These are source fixtures, not text screenshots or platform fonts.
    constexpr unsigned glyphs[2][7]={{31,16,16,30,16,16,31},{17,17,17,31,17,17,17}};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const unsigned gx=x%8,gy=y%11;
        const bool ink=pattern==0?x%3==0:pattern==1?(x+y)%3==0:
            gx<5&&gy<7&&((glyphs[(x/8)%2][gy]>>(4-gx))&1u);
        auto* p=rgba.data()+(std::size_t{y}*width+x)*4;p[0]=p[1]=p[2]=ink?0:255;p[3]=255;
    }
}

void gpu_quality_test(){
    constexpr unsigned target=128;constexpr std::size_t eye_bytes=target*target*4;
    Vulkan vk;vk.initialize();
    vk.image(vk.source,kSourceWidth,kSourceHeight,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,target,target,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.upload,kSourceWidth*kSourceHeight*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,eye_bytes*2,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    vk.begin();for(auto& image:vk.targets)vk.barrier(image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    unsigned source_width=kSourceWidth,source_height=kSourceHeight;bool initialized=false;
    std::vector<std::uint8_t> input(kSourceWidth*kSourceHeight*4);
    const auto upload=[&]{void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.upload.memory,0,input.size(),0,&mapped),"map_quality_source");std::memcpy(mapped,input.data(),input.size());vkUnmapMemory(vk.device,vk.upload.memory);
        vk.begin();vk.barrier(vk.source.image,initialized?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            initialized?VkAccessFlags{VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}:VkAccessFlags{0},VK_ACCESS_TRANSFER_WRITE_BIT,initialized?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={source_width,source_height,1};
        vkCmdCopyBufferToImage(vk.command,vk.upload.buffer,vk.source.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        vk.barrier(vk.source.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();initialized=true;
    };
    const auto read=[&]{
        vk.begin();for(unsigned eye=0;eye<2;++eye){auto image=vk.targets[eye].image;
            vk.barrier(image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};region.bufferOffset=eye_bytes*eye;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={target,target,1};
            vkCmdCopyImageToBuffer(vk.command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,vk.readback.buffer,1,&region);
            vk.barrier(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=vk.readback.buffer;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(vk.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);vk.submit();
        std::vector<std::uint8_t> out(eye_bytes*2);void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.readback.memory,0,out.size(),0,&mapped),"map_quality_result");std::memcpy(out.data(),mapped,out.size());vkUnmapMemory(vk.device,vk.readback.memory);
        require(std::memcmp(out.data(),out.data()+eye_bytes,eye_bytes)==0,"quality_equal_pose_eyes_share_display_phase");return out;
    };
    surfxr::UiPanel panel;panel.pose.position.z=-2;panel.width=16;panel.height=8;
    std::vector<surfxr::ProjectionSource> sources{{vk.source.image,vk.source.view,panel,kSourceWidth,kSourceHeight,0,1}};
    std::array<surfxr::ProjectionEye,2> eyes{};for(unsigned eye=0;eye<2;++eye){eyes[eye].target=vk.targets[eye].view;eyes[eye].fov={-.7853982f,.7853982f,.7853982f,-.7853982f};}
    surfxr::ProjectionRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,1,1);
    const auto reusable=[&]{const auto& source=sources[0];
        return renderer.can_reuse_source(source.cache_slot,source.width,source.height,source.content_generation);};
    require(!reusable()&&!renderer.can_reuse_source(1,kSourceWidth,kSourceHeight,1),"quality_uninitialized_cache_cannot_skip_upload");
    for(unsigned slot:{4u,1000u}){
        bool query_rejected=false,invalidate_rejected=false;
        try{renderer.can_reuse_source(slot,kSourceWidth,kSourceHeight,1);}catch(const std::runtime_error&){query_rejected=true;}
        try{renderer.invalidate(slot);}catch(const std::runtime_error&){invalidate_rejected=true;}
        require(query_rejected&&invalidate_rejected,"quality_cache_operations_reject_invalid_slots");
    }
    for(unsigned y=0;y<kSourceHeight;++y)for(unsigned x=0;x<kSourceWidth;++x){auto* p=input.data()+(y*kSourceWidth+x)*4;
        p[0]=static_cast<std::uint8_t>((x*17+y*29)%256);p[1]=static_cast<std::uint8_t>((x*43+y*7)%256);p[2]=static_cast<std::uint8_t>((x*3+y*61)%256);p[3]=255;}
    upload();unsigned reference_pixels=0;std::array<std::vector<std::uint8_t>,2> phase_images;
    for(unsigned phase=0;phase<2;++phase){renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,phase);phase_images[phase]=read();
        require(reusable(),"quality_completed_mips_allow_source_upload_reuse");
        for(unsigned y=52;y<76;y+=3)for(unsigned x=52;x<76;x+=3){
            const double du=4.0/(panel.width*target),dv=4.0/(panel.height*target);
            const auto expected=reference_small_quality(input,.5+(x+.5-target*.5)*du,.5+(y+.5-target*.5)*dv);
            const auto* actual=phase_images[phase].data()+(y*target+x)*4;
            for(unsigned c=0;c<4;++c)require(std::abs(int(actual[c])-int(expected[c]))<=3,"quality_magnification_matches_independent_bilinear_reference");
            ++reference_pixels;
        }
    }
    require(phase_images[0]==phase_images[1],"quality_static_pixels_are_display_phase_invariant");
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,1);require(read()==phase_images[1],"quality_repeat_has_no_temporal_history");
    auto stats=renderer.statistics();require(stats.mip_updates==1&&stats.mip_reuses==2&&stats.cached_levels==10,"quality_reuses_all_mips_for_repeated_video");
    require(!renderer.can_reuse_source(0,0,kSourceHeight,1)&&!renderer.can_reuse_source(0,kSourceWidth,0,1)&&
            !renderer.can_reuse_source(0,kSourceWidth,kSourceHeight,0),"quality_empty_cache_keys_cannot_skip_upload");
    require(!renderer.can_reuse_source(0,kSourceWidth+1,kSourceHeight,1)&&
            !renderer.can_reuse_source(0,kSourceWidth,kSourceHeight+1,1)&&
            !renderer.can_reuse_source(0,kSourceWidth,kSourceHeight,2)&&reusable(),"quality_cache_query_checks_full_key_without_invalidation");
    require(renderer.statistics().mip_updates==stats.mip_updates&&renderer.statistics().mip_reuses==stats.mip_reuses,
            "quality_cache_query_does_not_change_statistics");
    if(stats.gpu_timestamps){require(stats.mip_gpu_samples==1&&stats.projection_gpu_samples==3,"quality_gpu_timestamp_sample_counts");
        require(stats.mip_gpu_ms_sum>=0&&stats.projection_gpu_ms_sum>=0&&stats.last_mip_gpu_ms==0&&stats.last_projection_gpu_ms>=0,"quality_gpu_timing_reuse_has_zero_mip_cost");}
    renderer.render(sources,eyes,surfxr::ProjectionSampling::Baseline,2);
    require(reusable(),"quality_owned_cache_survives_baseline_render");
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,0);
    require(read()==phase_images[0]&&renderer.statistics().mip_updates==1,"quality_runtime_switch_preserves_owned_cache");
    sources[0].panel.width=2;sources[0].panel.height=1;
    const auto check_center=[&](const std::vector<std::uint8_t>& image,const std::array<unsigned,4>& expected,const char* label){
        for(unsigned y=58;y<70;++y)for(unsigned x=58;x<70;++x)for(unsigned c=0;c<4;++c)
            require(std::abs(int(image[(y*target+x)*4+c])-int(expected[c]))<=3,label);
    };
    for(unsigned y=0;y<source_height;++y)for(unsigned x=0;x<source_width;++x){auto* p=input.data()+(y*source_width+x)*4;p[0]=p[1]=p[2]=((x+y)&1)?255:0;p[3]=255;}
    upload();sources[0].content_generation=2;require(!reusable(),"quality_new_generation_requires_source_upload");
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,2);
    require(reusable(),"quality_new_generation_becomes_reusable_after_render");
    check_center(read(),{188,188,188,255},"quality_minification_uses_linear_light_mips");
    for(unsigned y=0;y<source_height;++y)for(unsigned x=0;x<source_width;++x){auto* p=input.data()+(y*source_width+x)*4;
        p[0]=255;p[1]=p[2]=p[3]=((x+y)&1)?255:0;}
    upload();sources[0].content_generation=3;renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,3);
    const auto transparent=read();check_center(transparent,{188,188,188,128},"quality_mips_exclude_transparent_red_without_double_alpha");
    for(std::size_t i=0;i<input.size();i+=4){input[i]=0;input[i+1]=255;input[i+2]=0;input[i+3]=255;}upload();
    require(reusable(),"quality_cache_key_does_not_depend_on_current_source_pixels");
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,3);
    require(read()==transparent&&renderer.statistics().mip_updates==3,"quality_unchanged_generation_never_rereads_source");
    sources[0].content_generation=4;renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,4);check_center(read(),{0,255,0,255},"quality_new_generation_updates_pixels");
    renderer.invalidate(0);require(!reusable(),"quality_invalidated_cache_requires_source_upload");
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,4);
    require(reusable(),"quality_invalidated_cache_recovers_after_render");
    require(renderer.statistics().mip_updates==5,"quality_explicit_source_invalidation");
    // Recreate the same slot at a new size without changing its generation.
    vkDestroyImageView(vk.device,vk.source.view,nullptr);vkDestroyImage(vk.device,vk.source.image,nullptr);vkFreeMemory(vk.device,vk.source.memory,nullptr);vk.source={};
    source_width=256;source_height=128;initialized=false;vk.image(vk.source,source_width,source_height,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    sources[0].image=vk.source.image;sources[0].view=vk.source.view;sources[0].width=source_width;sources[0].height=source_height;
    require(!reusable(),"quality_resized_source_cannot_reuse_previous_extent");
    input.resize(source_width*source_height*4);for(std::size_t i=0;i<input.size();i+=4){input[i]=input[i+1]=0;input[i+2]=input[i+3]=255;}upload();
    renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,5);check_center(read(),{0,0,255,255},"quality_resized_slot_rebuilds_cache");
    require(reusable()&&!renderer.can_reuse_source(0,kSourceWidth,kSourceHeight,4),"quality_resized_cache_only_reuses_new_extent");
    stats=renderer.statistics();require(stats.mip_updates==6&&stats.cached_levels==9&&stats.cached_bytes<256*128*6,"quality_cache_resize_and_full_chain_bound");
    // Compare moving minified lines and glyphs against an independent exact
    // box footprint. Report residual motion error rather than penalizing the
    // expected image change caused by moving the source under the pixel.
    double gpu_error=0,stable_error=0,legacy_error=0,gpu_motion_error=0,stable_motion_error=0,legacy_motion_error=0;
    double gpu_luminance=0,reference_luminance=0,stable_luminance=0,legacy_luminance=0;
    unsigned alias_pixels=0,motion_pixels=0,alias_reference_max=0;
    for(unsigned pattern=0;pattern<3;++pattern){
        grayscale_fixture(input,source_width,source_height,pattern);upload();sources[0].content_generation=10+pattern;
        const auto mips=reference_mips(input,source_width,source_height);
        for(double rho:{1.5,2.5,4.}){
            sources[0].panel.width=float(4.*source_width/(rho*target));sources[0].panel.height=float(4.*source_height/(rho*target));
            std::vector<std::array<double,3>> previous;
            for(unsigned step=0;step<8;++step){
                const double shift=step/8.;
                sources[0].panel.pose.position.x=float(shift*4./target);
                sources[0].panel.pose.position.y=float(-shift*4./target);
                renderer.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,step);const auto pixels=read();
                std::vector<std::array<double,3>> current;
                for(unsigned y=52;y<76;y+=3)for(unsigned x=44;x<84;x+=3){
                    const double u=.5+(x+.5-target*.5-shift)*rho/source_width,v=.5+(y+.5-target*.5-shift)*rho/source_height;
                    const double reference=reference_box(mips[0],u,v,rho),stable=reference_sample(mips,u,v,std::log2(rho));
                    const double legacy=legacy_quality_sample(mips,u,v,rho,step&1u);
                    const auto value=pixels[(y*target+x)*4+1];const double actual=linear_srgb(value);
                    gpu_luminance+=actual;reference_luminance+=reference;stable_luminance+=stable;legacy_luminance+=legacy;
                    const unsigned mismatch=std::abs(int(value)-int(encoded_srgb(stable)));alias_reference_max=std::max(alias_reference_max,mismatch);
                    require(mismatch<=3,"quality_minified_strokes_match_independent_trilinear_reference");
                    const std::array<double,3> residual{{actual-reference,stable-reference,legacy-reference}};
                    gpu_error+=residual[0]*residual[0];stable_error+=residual[1]*residual[1];legacy_error+=residual[2]*residual[2];++alias_pixels;
                    const auto index=current.size();current.push_back(residual);
                    if(step){for(unsigned c=0;c<3;++c){const auto delta=residual[c]-previous[index][c];
                        if(c==0)gpu_motion_error+=delta*delta;else if(c==1)stable_motion_error+=delta*delta;else legacy_motion_error+=delta*delta;}
                        ++motion_pixels;}
                }
                previous=std::move(current);
            }
        }
    }
    stats=renderer.statistics();
    // Sparse diagnostics must neither reuse an old duration on unsampled
    // frames nor alter their pixels. The selected repeat has no mip work.
    const auto stable_pixels=read();
    {
        surfxr::ProjectionRenderer sparse(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,1,3);
        for(unsigned frame=0;frame<4;++frame){
            sparse.render(sources,eyes,surfxr::ProjectionSampling::QualitySS,frame);
            require(read()==stable_pixels,"quality_sparse_timestamps_preserve_pixels");
            const auto sampled=sparse.statistics();
            if(frame==1||frame==2)require(sampled.last_mip_gpu_ms==-1&&sampled.last_projection_gpu_ms==-1,
                "quality_unsampled_durations_are_unavailable");
            if(sampled.gpu_timestamps){
                require(sampled.projection_gpu_samples==(frame<3?1u:2u)&&sampled.mip_gpu_samples==1,
                    "quality_sparse_timestamp_sample_counts");
                if(frame==3)require(sampled.last_mip_gpu_ms==0&&sampled.last_projection_gpu_ms>=0,
                    "quality_sampled_repeat_has_no_mip_sample");
            }
        }
    }
    std::cout<<"{\"status\":\"gpu_quality_passed\",\"reference_pixels\":"<<reference_pixels<<",\"mip_updates\":"<<stats.mip_updates
        <<",\"mip_reuses\":"<<stats.mip_reuses<<",\"quality_frames\":"<<stats.quality_frames<<",\"baseline_frames\":"<<stats.baseline_frames
        <<",\"cached_levels\":"<<stats.cached_levels<<",\"cached_bytes\":"<<stats.cached_bytes
        <<",\"static_phase_invariant\":true,\"alias_pixels\":"<<alias_pixels<<",\"alias_reference_max_code_error\":"<<alias_reference_max
        <<",\"alias_gpu_box_mse\":"<<gpu_error/alias_pixels<<",\"alias_stable_box_mse\":"<<stable_error/alias_pixels<<",\"alias_legacy_box_mse\":"<<legacy_error/alias_pixels
        <<",\"alias_gpu_mean_luminance\":"<<gpu_luminance/alias_pixels<<",\"alias_reference_mean_luminance\":"<<reference_luminance/alias_pixels
        <<",\"alias_stable_mean_luminance\":"<<stable_luminance/alias_pixels<<",\"alias_legacy_mean_luminance\":"<<legacy_luminance/alias_pixels
        <<",\"motion_residual_gpu_mse\":"<<gpu_motion_error/motion_pixels<<",\"motion_residual_stable_mse\":"<<stable_motion_error/motion_pixels<<",\"motion_residual_legacy_mse\":"<<legacy_motion_error/motion_pixels
        <<",\"gpu_timestamps\":"<<(stats.gpu_timestamps?"true":"false")<<",\"mip_gpu_samples\":"<<stats.mip_gpu_samples<<",\"projection_gpu_samples\":"<<stats.projection_gpu_samples
        <<",\"network_used\":false,\"openxr_session_started\":false}\n";
}

void gpu_benchmark(unsigned target,surfxr::ProjectionSampling sampling,bool dirty,unsigned frames,bool timing){
    constexpr unsigned warmup=12;constexpr std::size_t source_bytes=2560*1440*4;
    const std::array<std::array<unsigned,2>,3> dimensions{{{{2560,1440}},{{2560,1440}},{{1440,2560}}}};
    Vulkan vk;vk.initialize();const std::array<Image*,3> images{{&vk.source,&vk.extra_sources[0],&vk.extra_sources[1]}};
    for(unsigned i=0;i<images.size();++i)vk.image(*images[i],dimensions[i][0],dimensions[i][1],VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,target,target,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.upload,source_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);vk.buffer(vk.readback,8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<std::uint8_t> pixels(source_bytes);
    // Source upload is setup, outside measurements. Changing generation forces
    // the complete mip rebuild workload on stable synthetic pixels; this does
    // not include decode/CPU staging or claim a measured display refresh rate.
    for(unsigned source=0;source<images.size();++source){
        std::uint32_t random=0x12345678u+source;
        for(std::size_t i=0;i<pixels.size();i+=4){random^=random<<13;random^=random>>17;random^=random<<5;
            pixels[i]=static_cast<std::uint8_t>(random);pixels[i+1]=static_cast<std::uint8_t>(random>>8);pixels[i+2]=static_cast<std::uint8_t>(random>>16);pixels[i+3]=255;}
        void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.upload.memory,0,source_bytes,0,&mapped),"map_benchmark_upload");std::memcpy(mapped,pixels.data(),source_bytes);vkUnmapMemory(vk.device,vk.upload.memory);
        vk.begin();vk.barrier(images[source]->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={dimensions[source][0],dimensions[source][1],1};
        vkCmdCopyBufferToImage(vk.command,vk.upload.buffer,images[source]->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        vk.barrier(images[source]->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    }
    vk.begin();for(auto& image:vk.targets)vk.barrier(image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    std::vector<surfxr::ProjectionSource> sources;
    for(unsigned i=0;i<images.size();++i){surfxr::UiPanel panel;panel.panel_id=i;panel.source_id=i;
        panel.width=i==2?1.125f:2.f;panel.height=i==2?2.f:1.125f;panel.curvature_radians=.8f;
        panel.pose.position={i==1?-1.8f:(i==2?1.7f:0.f),0,i? -2.35f:-2.f};
        const float yaw=i==1?.45f:(i==2?-.45f:0.f);panel.pose.orientation={0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
        sources.push_back({images[i]->image,images[i]->view,panel,dimensions[i][0],dimensions[i][1],i,1});
    }
    std::array<surfxr::ProjectionEye,2> eyes{};for(unsigned i=0;i<2;++i){eyes[i].target=vk.targets[i].view;eyes[i].pose.position.x=i? .032f:-.032f;eyes[i].fov={-.7853982f,.7853982f,.7853982f,-.7853982f};}
    const unsigned timestamp_period=timing?1u:0u;
    surfxr::ProjectionRenderer renderer(vk.physical,vk.device,vk.queue,vk.family,kFormat,target,target,1,timestamp_period);
    std::vector<double> waits,wall;waits.reserve(frames);wall.reserve(frames);surfxr::ProjectionSamplingStats before;
    for(unsigned frame=0;frame<warmup+frames;++frame){
        if(frame==warmup)before=renderer.statistics();
        if(dirty)for(auto& source:sources)source.content_generation=std::uint64_t{frame}+1;
        const auto start=std::chrono::steady_clock::now();renderer.render(sources,eyes,sampling,frame);
        if(frame>=warmup){wall.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());waits.push_back(renderer.last_gpu_wait_ms());}
    }
    const auto after=renderer.statistics();const auto updates=after.mip_updates-before.mip_updates,reuses=after.mip_reuses-before.mip_reuses;
    const auto mip_gpu_samples=after.mip_gpu_samples-before.mip_gpu_samples,projection_gpu_samples=after.projection_gpu_samples-before.projection_gpu_samples;
    if(!timing)require(!after.gpu_timestamps&&!mip_gpu_samples&&!projection_gpu_samples&&
        after.last_mip_gpu_ms==-1&&after.last_projection_gpu_ms==-1,"benchmark_disabled_timestamps_have_no_samples");
    if(after.gpu_timestamps){
        require(projection_gpu_samples==frames,"benchmark_projection_gpu_timestamp_samples");
        require(mip_gpu_samples==(sampling==surfxr::ProjectionSampling::QualitySS&&dirty?frames:0u),"benchmark_mip_gpu_samples_only_when_updated");
    }
    if(sampling==surfxr::ProjectionSampling::QualitySS)require(updates==(dirty?frames*3u:0u)&&reuses==(dirty?0u:frames*3u),"benchmark_exercises_requested_mip_workload");
    else require(!updates&&!reuses,"baseline_benchmark_has_no_mip_work");
    // Check one opaque center pixel per eye without a full-image readback/file.
    vk.begin();for(unsigned i=0;i<2;++i){vk.barrier(vk.targets[i].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};region.bufferOffset=i*4;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageOffset={static_cast<int>(target/2),static_cast<int>(target/2),0};region.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(vk.command,vk.targets[i].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,vk.readback.buffer,1,&region);}
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=vk.readback.buffer;host.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(vk.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);vk.submit();
    void* mapped=nullptr;checked(vkMapMemory(vk.device,vk.readback.memory,0,8,0,&mapped),"map_benchmark_result");const auto* result=static_cast<const std::uint8_t*>(mapped);
    const bool rendered=result[3]==255&&result[7]==255;vkUnmapMemory(vk.device,vk.readback.memory);require(rendered,"benchmark_both_eyes_rendered");
    const auto summary=[](std::vector<double> values){double sum=0;for(double value:values){require(std::isfinite(value)&&value>=0,"benchmark_finite_timing");sum+=value;}std::sort(values.begin(),values.end());
        const auto percentile=[&](double p){return values[static_cast<std::size_t>(std::ceil(values.size()*p))-1];};
        return std::array<double,4>{{sum/values.size(),percentile(.5),percentile(.95),values.back()}};};
    const auto w=summary(waits),total=summary(wall);
    std::cout<<"{\"status\":\"gpu_benchmark_passed\",\"sampling\":\""<<(sampling==surfxr::ProjectionSampling::QualitySS?"quality":"baseline")
        <<"\",\"dirty_sources\":"<<(dirty?"true":"false")<<",\"source_dimensions\":[[2560,1440],[2560,1440],[1440,2560]],\"eye_width\":"<<target<<",\"eye_height\":"<<target
        <<",\"samples\":1,\"warmup_frames\":"<<warmup<<",\"measured_frames\":"<<frames<<",\"gpu_wait_ms_mean\":"<<w[0]<<",\"gpu_wait_ms_median\":"<<w[1]<<",\"gpu_wait_ms_p95\":"<<w[2]<<",\"gpu_wait_ms_max\":"<<w[3]
        <<",\"render_wall_ms_mean\":"<<total[0]<<",\"render_wall_ms_median\":"<<total[1]<<",\"render_wall_ms_p95\":"<<total[2]<<",\"render_wall_ms_max\":"<<total[3]
        <<",\"measured_mip_updates\":"<<updates<<",\"measured_mip_reuses\":"<<reuses<<",\"cached_bytes\":"<<after.cached_bytes<<",\"cached_levels\":"<<after.cached_levels
        <<",\"timestamp_period\":"<<timestamp_period<<",\"gpu_timestamps\":"<<(after.gpu_timestamps?"true":"false")<<",\"mip_gpu_samples\":"<<mip_gpu_samples<<",\"projection_gpu_samples\":"<<projection_gpu_samples
        <<",\"mip_gpu_ms_mean\":"<<(mip_gpu_samples?(after.mip_gpu_ms_sum-before.mip_gpu_ms_sum)/mip_gpu_samples:-1.)
        <<",\"projection_gpu_ms_mean\":"<<(projection_gpu_samples?(after.projection_gpu_ms_sum-before.projection_gpu_ms_sum)/projection_gpu_samples:-1.)
        <<",\"upstream_decode_upload_included\":false,\"network_used\":false,\"openxr_session_started\":false}\n";
}
}
