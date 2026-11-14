#include "foveation_gpu_test_cases.hpp"
namespace surfxr::tests::foveation_gpu {
void run_sharpen(unsigned sw,unsigned sh,unsigned width,unsigned height,float sharpness,unsigned frames,bool quad,
                 bool zero_weight_skip,const std::string& pattern){
    Vulkan vk;vk.initialize(quad);
    for(auto& image:vk.sources)vk.image(image,sw,sh,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for(auto& image:vk.targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,true);
    for(auto& image:vk.extra_targets)vk.image(image,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,true);
    vk.buffer(vk.upload,std::size_t{sw}*sh*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    vk.buffer(vk.readback,std::size_t{width}*height*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto source_bytes=std::size_t{sw}*sh*4,eye_bytes=std::size_t{width}*height*4;
    std::vector<std::uint8_t> encoded(source_bytes*2);
    for(unsigned eye=0;eye<2;++eye){
        auto fixture=surfxr::enhancement_reference::fixture(sw,sh,eye);
        // Include dark/bright transfer boundaries as well as the asymmetric
        // checker pattern; filtering must happen before E's 8-bit quantization.
        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw/8;++x){
            const auto offset=(std::size_t(y)*sw+x)*4;
            const auto shade=static_cast<std::uint8_t>(x*255/std::max(1u,sw/8-1));
            fixture.rgba[offset]=shade;fixture.rgba[offset+1]=shade;fixture.rgba[offset+2]=shade;
        }
        std::uint32_t random=0x31415927u+eye;
        constexpr std::array<unsigned,10> boundary_codes{{0,1,10,11,63,64,127,128,254,255}};
        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x){
            auto* pixel=fixture.rgba.data()+(std::size_t(y)*sw+x)*4;
            if(pattern=="nonsaturated"){
                // Linear-light filtering of these mid-gray codes cannot make
                // saturated E texels. Every candidate lane must take the full
                // path, exposing branch overhead without changing the workload.
                for(unsigned channel=0;channel<3;++channel){random^=random<<13;random^=random>>17;random^=random<<5;
                    pixel[channel]=static_cast<std::uint8_t>(96u+(random&63u));}
            }else if(pattern=="stress"){
                // Separate from the historical mixed benchmark: saturated G
                // regions with nontrivial R/B and adjacent E rounding codes.
                // The perimeter exercises clamped neighbors/helper invocations
                // for both even and odd complete-eye dimensions.
                if(x<2||y<2||x+2>=sw||y+2>=sh)pixel[1]=((x/4+y/4+eye)&1u)?0:255;
                else if(y<sh/4&&x<sw/4)pixel[1]=((x/4+y/4+eye)&1u)?0:255;
                else if(y<sh/4&&x<sw/2){const auto shade=static_cast<std::uint8_t>(boundary_codes[x%boundary_codes.size()]);
                    pixel[0]=pixel[1]=pixel[2]=shade;}
            }
            pixel[3]=255;
        }
        std::copy(fixture.rgba.begin(),fixture.rgba.end(),encoded.begin()+eye*source_bytes);
    }
    vk.upload_pixels(encoded,sw,sh,true);
    vk.begin();for(auto& image:vk.extra_targets)vk.barrier(image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);vk.submit();
    surfxr::FoveationRenderer reconstruct(vk.physical,vk.device,vk.queue,vk.family,format,width,height,quad,zero_weight_skip);
    // This is the same full-float quad/scalar path with only specialization 2
    // inverted. It preserves a real legacy comparison even if both accelerated
    // and portable production shaders gain the same optimization.
    surfxr::FoveationRenderer opposite(vk.physical,vk.device,vk.queue,vk.family,format,width,height,quad,!zero_weight_skip);
    require(reconstruct.zero_weight_skip_enabled()==zero_weight_skip&&opposite.zero_weight_skip_enabled()!=zero_weight_skip,
            "zero_weight_skip_selection_not_applied");
    require(reconstruct.supports_quad_sharpening()==opposite.supports_quad_sharpening(),"zero_weight_skip_comparison_changed_quad_mode");
    const bool quad_available=quad&&sharpness>0&&reconstruct.supports_quad_sharpening();
    std::unique_ptr<surfxr::FoveationRenderer> scalar;
    if(quad_available)scalar=std::make_unique<surfxr::FoveationRenderer>(vk.physical,vk.device,vk.queue,vk.family,format,width,height,false,zero_weight_skip);
    surfxr::ImageEnhancementRenderer enhance(vk.physical,vk.device,vk.queue,vk.family,format,width,height,width,height);
    std::array<surfxr::FoveationEye,2> original{},fused{};
    std::array<surfxr::ImageEnhancementEye,2> separate{};
    for(unsigned eye=0;eye<2;++eye){
        original[eye]={vk.sources[eye].image,vk.sources[eye].view,vk.targets[eye].view,true};
        fused[eye]={vk.sources[eye].image,vk.sources[eye].view,vk.extra_targets[eye].view,true};
        separate[eye]={vk.targets[eye].image,vk.targets[eye].view,vk.extra_targets[eye].view,true};
        if(enhance.supports_raw_views()){
            separate[eye].raw_source_view=vk.targets[eye].raw_view;
            separate[eye].raw_target=vk.extra_targets[eye].raw_view;
        }
    }
    require(reconstruct.render(original),"sharpen_reference_reconstruction_skipped");
    const auto complete=vk.read_pixels(width,height);
    for(unsigned eye=0;eye<2;++eye)fused[eye].raw_target=vk.extra_targets[eye].raw_view;
    require(reconstruct.render(fused,true,0),"zero_strength_reconstruction_skipped");
    require(vk.read_pixels(width,height,true)==complete,"zero_strength_changed_reconstruction_bytes");
    reconstruct.complete(true);require(reconstruct.last_gpu_wait_ms()==0,"zero_strength_covered_waited");
    std::array<surfxr::enhancement_reference::Image,2> complete_images;
    for(unsigned eye=0;eye<2;++eye)complete_images[eye]={width,height,std::vector<std::uint8_t>(complete.begin()+eye*eye_bytes,complete.begin()+(eye+1)*eye_bytes)};
    const bool full_compare=std::uint64_t{width}*height<=65536;
    const bool fused_e_diagnostic=reconstruct.supports_raw_views();
    std::vector<std::uint8_t> fused_e;
    std::array<surfxr::enhancement_reference::Image,2> fused_e_images;
    double fused_e_error_sum=0;unsigned fused_e_error_max=0;
    std::uint64_t fused_e_saturated_green_pixels=0;
    if(fused_e_diagnostic){
        // Force the positive-strength shader but make the sharpening delta
        // negligible. Its raw output exposes the quantized E samples used by
        // fusion. This extra diagnostic is outside all timing measurements.
        require(reconstruct.render(fused,true,1e-8f),"fused_E_diagnostic_skipped");
        fused_e=vk.read_pixels(width,height,true);reconstruct.complete(true);
        for(std::size_t i=0;i<fused_e.size();++i)if(i%4!=3){
            const unsigned error=std::abs(int(fused_e[i])-int(complete[i]));
            fused_e_error_sum+=error;fused_e_error_max=std::max(fused_e_error_max,error);
            if(i%4==1&&(fused_e[i]==0||fused_e[i]==255))++fused_e_saturated_green_pixels;
        }
        require(pattern=="nonsaturated"?fused_e_saturated_green_pixels==0:
            fused_e_saturated_green_pixels>0&&fused_e_saturated_green_pixels<std::uint64_t{width}*height*2,
            "fixture_did_not_cover_requested_zero_weight_conditions");
        for(unsigned eye=0;eye<2;++eye)fused_e_images[eye]={width,height,std::vector<std::uint8_t>(fused_e.begin()+eye*eye_bytes,fused_e.begin()+(eye+1)*eye_bytes)};
    }
    double separate_gpu_sum=0,separate_wait_sum=0;unsigned separate_gpu_samples=0;
    constexpr unsigned warmup=3;
    for(unsigned i=0;i<warmup+frames;++i){
        require(reconstruct.render(original,true),"separate_reconstruction_skipped");
        const bool enhanced=enhance.render(separate,{false,sharpness},true);
        if(enhanced)enhance.complete(false);
        reconstruct.complete(enhanced);
        const auto a=reconstruct.last_gpu_duration_ms(),b=enhanced?enhance.last_gpu_duration_ms():0;
        if(i>=warmup){
            if(a>=0&&b>=0){separate_gpu_sum+=a+b;++separate_gpu_samples;}
            separate_wait_sum+=reconstruct.last_gpu_wait_ms()+(enhanced?enhance.last_gpu_wait_ms():0);
        }
    }
    const auto two_pass=sharpness>0?vk.read_pixels(width,height,true):complete;
    std::vector<std::array<unsigned,2>> probes;
    if(full_compare){for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)probes.push_back({x,y});}
    else{for(unsigned y=0;y<=24;++y)for(unsigned x=0;x<=32;++x)probes.push_back({(width-1)*x/32,(height-1)*y/24});}
    bool pair_colors_valid=true,cpu_colors_valid=true,accelerated_colors_valid=true,fused_e_colors_valid=true,skip_colors_valid=true;
    for(bool raw:{false,true}){
        if(raw&&!reconstruct.supports_raw_views())continue;
        const bool quad_enabled=quad_available;
        const bool compare_scalar=scalar&&quad_enabled;
        for(unsigned eye=0;eye<2;++eye){fused[eye].dirty=true;fused[eye].raw_target=raw?vk.extra_targets[eye].raw_view:VK_NULL_HANDLE;}
        double gpu_sum=0,gpu_max=0,wait_sum=0;unsigned gpu_samples=0;
        for(unsigned i=0;i<warmup+frames;++i){
            require(reconstruct.render(fused,true,sharpness),"fused_sharpen_skipped");
            require(reconstruct.last_gpu_wait_ms()==0,"fused_defer_waited_on_host");
            reconstruct.complete(false);const auto gpu=reconstruct.last_gpu_duration_ms();
            require(gpu==-1||(std::isfinite(gpu)&&gpu>=0),"fused_invalid_gpu_timestamp");
            if(i>=warmup){wait_sum+=reconstruct.last_gpu_wait_ms();if(gpu>=0){gpu_sum+=gpu;gpu_max=std::max(gpu_max,gpu);++gpu_samples;}}
        }
        const auto pixels=vk.read_pixels(width,height,true);
        double skip_error_sum=0;unsigned skip_max=0;bool skip_byte_identity=false;
        {
            // The opposite option is never included in the selected path's
            // timing. Release its readback before allocating scalar comparison
            // pixels, keeping the full-size test's peak host storage bounded.
            require(opposite.render(fused,false,sharpness),"opposite_zero_weight_skip_reference_skipped");
            const auto opposite_pixels=vk.read_pixels(width,height,true);
            skip_byte_identity=pixels==opposite_pixels;
            for(std::size_t i=3;i<opposite_pixels.size();i+=4)require(opposite_pixels[i]==255,"opposite_zero_weight_skip_alpha_not_opaque");
            for(unsigned eye=0;eye<2;++eye)for(const auto& probe:probes){
                const auto offset=eye*eye_bytes+(std::size_t(probe[1])*width+probe[0])*4;
                for(unsigned channel=0;channel<4;++channel){const unsigned error=std::abs(int(pixels[offset+channel])-int(opposite_pixels[offset+channel]));
                    skip_error_sum+=error;skip_max=std::max(skip_max,error);}
            }
        }
        std::vector<std::uint8_t> scalar_pixels;bool scalar_byte_identity=false;
        std::size_t scalar_different_bytes=0,scalar_different_pixels=0,scalar_first_byte=0;
        unsigned scalar_full_max_error=0;
        if(compare_scalar){
            require(scalar->render(fused,false,sharpness),"scalar_fused_reference_skipped");
            scalar_pixels=vk.read_pixels(width,height,true);
            scalar_byte_identity=pixels==scalar_pixels;
            // Diagnostic scan runs only after a full-byte failure, outside all
            // timings. Preserve the exact-equality gate and sampled statistics.
            if(!scalar_byte_identity)for(std::size_t i=0;i<pixels.size();i+=4){
                bool pixel_differs=false;
                for(unsigned channel=0;channel<4;++channel){
                    const auto byte=i+channel;
                    const unsigned error=std::abs(int(pixels[byte])-int(scalar_pixels[byte]));
                    if(error){if(!scalar_different_bytes)scalar_first_byte=byte;
                        ++scalar_different_bytes;pixel_differs=true;scalar_full_max_error=std::max(scalar_full_max_error,error);}
                }
                scalar_different_pixels+=pixel_differs;
            }
        }
        // Restore the selected variant before dirty/cache lifetime checks.
        require(reconstruct.render(fused,false,sharpness),"selected_fused_variant_restore_skipped");
        for(std::size_t i=3;i<pixels.size();i+=4)require(pixels[i]==255,"fused_alpha_not_opaque");
        double pair_error_sum=0,cpu_error_sum=0,quad_error_sum=0,fused_cpu_error_sum=0;
        unsigned pair_max=0,cpu_max=0,quad_max=0,fused_cpu_max=0,changed=0;
        std::array<unsigned,6> peak{};
        for(unsigned eye=0;eye<2;++eye)for(const auto& probe:probes){
            const auto x=probe[0],y=probe[1];const auto offset=eye*eye_bytes+(std::size_t(y)*width+x)*4;
            const auto golden=surfxr::enhancement_reference::enhance(complete_images[eye],(x+.5f)/width,(y+.5f)/height,{false,sharpness});
            const auto fused_golden=fused_e_diagnostic?surfxr::enhancement_reference::enhance(fused_e_images[eye],(x+.5f)/width,(y+.5f)/height,{false,sharpness}):surfxr::enhancement_reference::Color{};
            for(unsigned channel=0;channel<3;++channel){
                const unsigned pair_error=std::abs(int(pixels[offset+channel])-int(two_pass[offset+channel]));
                const unsigned cpu_error=std::abs(int(pixels[offset+channel])-int(std::lround(golden[channel]*255)));
                pair_error_sum+=pair_error;cpu_error_sum+=cpu_error;cpu_max=std::max(cpu_max,cpu_error);
                if(pair_error>pair_max){pair_max=pair_error;peak={eye,x,y,channel,pixels[offset+channel],two_pass[offset+channel]};}
                if(compare_scalar){const unsigned error=std::abs(int(pixels[offset+channel])-int(scalar_pixels[offset+channel]));
                    quad_error_sum+=error;quad_max=std::max(quad_max,error);}
                if(fused_e_diagnostic){const unsigned error=std::abs(int(pixels[offset+channel])-int(std::lround(fused_golden[channel]*255)));
                    fused_cpu_error_sum+=error;fused_cpu_max=std::max(fused_cpu_max,error);}
            }
            changed+=std::abs(int(pixels[offset+1])-int(complete[offset+1]))>=2;
        }
        const double pair_mean=pair_error_sum/(probes.size()*6.),cpu_mean=cpu_error_sum/(probes.size()*6.);
        // The two reconstructions can round E differently by one code. At
        // strength 1, a center G of 139 vs 140 was independently reproduced as
        // output G 142 vs 145 with identical neighbors. Permit amplification
        // only when E's bound and the independent sharpen-on-fused-E bound hold.
        const bool quantization_verified=fused_e_diagnostic&&fused_e_error_max<=1&&fused_cpu_max<=1;
        const unsigned two_pass_limit=quantization_verified?4:2;
        std::array<std::array<unsigned,3>,5> peak_complete{},peak_fused_complete{};
        constexpr std::array<std::array<int,2>,5> offsets{{{{0,0}},{{0,-1}},{{0,1}},{{-1,0}},{{1,0}}}};
        for(unsigned neighbor=0;neighbor<offsets.size();++neighbor){
            const auto x=static_cast<unsigned>(std::clamp(int(peak[1])+offsets[neighbor][0],0,int(width)-1));
            const auto y=static_cast<unsigned>(std::clamp(int(peak[2])+offsets[neighbor][1],0,int(height)-1));
            const auto offset=peak[0]*eye_bytes+(std::size_t(y)*width+x)*4;
            for(unsigned channel=0;channel<3;++channel){
                peak_complete[neighbor][channel]=complete[offset+channel];
                if(fused_e_diagnostic)peak_fused_complete[neighbor][channel]=fused_e[offset+channel];
            }
        }
        const auto peak_offset=peak[0]*eye_bytes+(std::size_t(peak[2])*width+peak[1])*4;
        const auto peak_golden=surfxr::enhancement_reference::enhance(complete_images[peak[0]],(peak[1]+.5f)/width,(peak[2]+.5f)/height,{false,sharpness});
        const auto fused_e_golden=fused_e_diagnostic?surfxr::enhancement_reference::enhance(fused_e_images[peak[0]],(peak[1]+.5f)/width,(peak[2]+.5f)/height,{false,sharpness}):surfxr::enhancement_reference::Color{};
        // Check clean and single-eye paths with changed content/strength, so an
        // accidental redraw of the other eye cannot pass by identical pixels.
        fused[0].dirty=fused[1].dirty=false;
        require(!reconstruct.render(fused,true,sharpness),"fused_clean_pair_submitted");
        require(vk.read_pixels(width,height,true)==pixels,"fused_clean_pair_changed_pixels");
        fused[0].dirty=true;const float alternate=sharpness>=.5f?0.f:1.f;
        require(reconstruct.render(fused,true,alternate),"fused_single_eye_skipped");
        const auto partial=vk.read_pixels(width,height,true);reconstruct.complete(true);
        require(reconstruct.last_gpu_wait_ms()==0,"fused_covered_completion_waited");
        require(std::equal(pixels.begin()+eye_bytes,pixels.end(),partial.begin()+eye_bytes),"fused_clean_right_eye_changed");
        require(!std::equal(pixels.begin(),pixels.begin()+eye_bytes,partial.begin()),"fused_dirty_left_eye_unchanged");
        fused[1].dirty=true;
        require(reconstruct.render(fused,true,sharpness)&&reconstruct.render(fused,true,sharpness),"fused_pending_reuse_skipped");
        require(vk.read_pixels(width,height,true)==pixels,"fused_pending_reuse_changed_pixels");reconstruct.complete(true);
        const auto duration=reconstruct.last_gpu_duration_ms();reconstruct.complete(false);
        require(reconstruct.last_gpu_duration_ms()==duration,"fused_duplicate_complete_changed_timestamp");
        const unsigned scalar_first_eye=static_cast<unsigned>(scalar_first_byte/eye_bytes);
        const unsigned scalar_first_x=static_cast<unsigned>((scalar_first_byte%eye_bytes)/4%width);
        const unsigned scalar_first_y=static_cast<unsigned>((scalar_first_byte%eye_bytes)/4/width);
        const auto write_scalar_neighborhood=[&](const std::vector<std::uint8_t>& bytes){
            if(!scalar_different_bytes||bytes.empty()){std::cout<<"null";return;}
            std::cout<<'[';
            for(unsigned neighbor=0;neighbor<offsets.size();++neighbor){
                if(neighbor)std::cout<<',';
                const auto x=static_cast<unsigned>(std::clamp(int(scalar_first_x)+offsets[neighbor][0],0,int(width)-1));
                const auto y=static_cast<unsigned>(std::clamp(int(scalar_first_y)+offsets[neighbor][1],0,int(height)-1));
                const auto offset=scalar_first_eye*eye_bytes+(std::size_t(y)*width+x)*4;
                std::cout<<'['<<unsigned(bytes[offset])<<','<<unsigned(bytes[offset+1])<<','<<unsigned(bytes[offset+2])<<']';
            }
            std::cout<<']';
        };
        std::cout<<"{\"fused_sharpen_gpu_case\":true,\"source_width\":"<<sw<<",\"source_height\":"<<sh<<",\"width\":"<<width<<",\"height\":"<<height
            <<",\"sharpness\":"<<sharpness<<",\"raw_target\":"<<(raw?"true":"false")<<",\"reference_raw_views\":"<<(enhance.supports_raw_views()?"true":"false")
            <<",\"pattern\":\""<<pattern<<"\",\"zero_weight_skip_enabled\":"<<(reconstruct.zero_weight_skip_enabled()?"true":"false")
            <<",\"zero_weight_skip_pixels_checked\":"<<probes.size()*2<<",\"zero_weight_skip_max_error\":"<<skip_max
            <<",\"zero_weight_skip_mean_error\":"<<skip_error_sum/(probes.size()*8.)
            <<",\"zero_weight_skip_bytes_checked\":"<<pixels.size()<<",\"zero_weight_skip_full_byte_identity\":"<<(skip_byte_identity?"true":"false")
            <<",\"quad_requested\":"<<(quad?"true":"false")<<",\"quad_enabled\":"<<(quad_enabled?"true":"false")
            <<",\"quad_scalar_pixels_checked\":"<<(compare_scalar&&quad_enabled?probes.size()*2:0)
            <<",\"quad_scalar_mean_error\":"<<(compare_scalar&&quad_enabled?quad_error_sum/(probes.size()*6.):-1)<<",\"quad_scalar_max_error\":"<<(compare_scalar&&quad_enabled?int(quad_max):-1)
            <<",\"quad_scalar_bytes_checked\":"<<(compare_scalar?pixels.size():0)<<",\"quad_scalar_full_byte_identity\":"<<(compare_scalar?(scalar_byte_identity?"true":"false"):"null")
            <<",\"quad_scalar_full_different_bytes\":"<<scalar_different_bytes<<",\"quad_scalar_full_different_pixels\":"<<scalar_different_pixels
            <<",\"quad_scalar_full_max_error\":"<<(compare_scalar?int(scalar_full_max_error):-1)
            <<",\"reference_pixels_checked\":"<<probes.size()*2<<",\"full_reference_compare\":"<<(full_compare?"true":"false")
            <<",\"two_pass_mean_error\":"<<pair_mean<<",\"two_pass_max_error\":"<<pair_max<<",\"cpu_mean_error\":"<<cpu_mean<<",\"cpu_max_error\":"<<cpu_max
            <<",\"peak_error_sample\":["<<peak[0]<<','<<peak[1]<<','<<peak[2]<<','<<peak[3]<<','<<peak[4]<<','<<peak[5]<<']'
            <<",\"peak_E_neighbor_order\":[\"center\",\"north\",\"south\",\"west\",\"east\"],\"peak_E_rgb\":[";
        for(unsigned neighbor=0;neighbor<peak_complete.size();++neighbor){if(neighbor)std::cout<<',';
            std::cout<<'['<<peak_complete[neighbor][0]<<','<<peak_complete[neighbor][1]<<','<<peak_complete[neighbor][2]<<']';}
        std::cout<<"],\"peak_two_pass_rgb\":["<<unsigned(two_pass[peak_offset])<<','<<unsigned(two_pass[peak_offset+1])<<','<<unsigned(two_pass[peak_offset+2])<<']'
            <<",\"peak_fused_rgb\":["<<unsigned(pixels[peak_offset])<<','<<unsigned(pixels[peak_offset+1])<<','<<unsigned(pixels[peak_offset+2])<<']'
            <<",\"peak_cpu_rgb\":["<<std::lround(peak_golden[0]*255)<<','<<std::lround(peak_golden[1]*255)<<','<<std::lround(peak_golden[2]*255)<<']'
            <<",\"fused_E_diagnostic\":"<<(fused_e_diagnostic?"true":"false")
            <<",\"fused_E_saturated_green_pixels\":"<<fused_e_saturated_green_pixels
            <<",\"fused_E_reconstruction_mean_error\":"<<(fused_e_diagnostic?fused_e_error_sum/(double(width)*height*6):-1)
            <<",\"fused_E_reconstruction_max_error\":"<<(fused_e_diagnostic?int(fused_e_error_max):-1)
            <<",\"fused_E_cpu_sharpen_mean_error\":"<<(fused_e_diagnostic?fused_cpu_error_sum/(probes.size()*6.):-1)
            <<",\"fused_E_cpu_sharpen_max_error\":"<<(fused_e_diagnostic?int(fused_cpu_max):-1)
            <<",\"two_pass_max_error_limit\":"<<two_pass_limit<<",\"quantization_verified\":"<<(quantization_verified?"true":"false")
            <<",\"peak_fused_E_rgb\":";
        if(fused_e_diagnostic){std::cout<<'[';
            for(unsigned neighbor=0;neighbor<peak_fused_complete.size();++neighbor){if(neighbor)std::cout<<',';
                std::cout<<'['<<peak_fused_complete[neighbor][0]<<','<<peak_fused_complete[neighbor][1]<<','<<peak_fused_complete[neighbor][2]<<']';}
            std::cout<<']';
        }else std::cout<<"null";
        std::cout<<",\"peak_fused_E_sharpen_cpu_rgb\":";
        if(fused_e_diagnostic)std::cout<<'['<<std::lround(fused_e_golden[0]*255)<<','<<std::lround(fused_e_golden[1]*255)<<','<<std::lround(fused_e_golden[2]*255)<<']';
        else std::cout<<"null";
        std::cout<<",\"quad_scalar_first_difference_order\":[\"eye\",\"x\",\"y\",\"channel\",\"quad_byte\",\"scalar_byte\"],\"quad_scalar_first_difference\":";
        if(scalar_different_bytes)std::cout<<'['<<scalar_first_eye<<','<<scalar_first_x<<','<<scalar_first_y<<','<<scalar_first_byte%4<<','
            <<unsigned(pixels[scalar_first_byte])<<','<<unsigned(scalar_pixels[scalar_first_byte])<<']';
        else std::cout<<"null";
        std::cout<<",\"quad_scalar_neighbor_order\":[\"center\",\"north\",\"south\",\"west\",\"east\"],\"quad_scalar_first_quad_rgb\":";
        write_scalar_neighborhood(pixels);
        std::cout<<",\"quad_scalar_first_scalar_rgb\":";write_scalar_neighborhood(scalar_pixels);
        std::cout<<",\"quad_scalar_first_E_rgb\":";write_scalar_neighborhood(complete);
        std::cout<<",\"quad_scalar_first_fused_E_rgb\":";write_scalar_neighborhood(fused_e);
        std::cout
            <<",\"changed_samples\":"<<changed
            <<",\"zero_strength_byte_identity\":true,\"dirty_and_defer_checked\":true,\"frames\":"<<frames
            <<",\"two_pass_gpu_samples\":"<<separate_gpu_samples<<",\"two_pass_gpu_mean_ms\":"<<(separate_gpu_samples?separate_gpu_sum/separate_gpu_samples:-1)
            <<",\"two_pass_wait_mean_ms\":"<<separate_wait_sum/frames<<",\"fused_gpu_samples\":"<<gpu_samples
            <<",\"fused_gpu_mean_ms\":"<<(gpu_samples?gpu_sum/gpu_samples:-1)<<",\"fused_gpu_max_ms\":"<<(gpu_samples?gpu_max:-1)
            <<",\"fused_wait_mean_ms\":"<<wait_sum/frames<<",\"network_used\":false,\"openxr_session_started\":false}\n";
        // Collect both target-view paths before reporting a color failure so
        // fallback transfer errors can be distinguished from sample geometry.
        pair_colors_valid&=pair_mean<=.35&&pair_max<=two_pass_limit;
        cpu_colors_valid&=cpu_mean<=1&&cpu_max<=8;
        // The original HEAD shader also has rare one-code quad/scalar differences
        // in vector/scalar reconstruction contexts (recorded in
        // perf-vr-sharp2-original-shader-nonsaturated-20261003.json). Preserve the
        // original exact sampled check and add a full-image one-code bound;
        // quad/scalar full-byte identity is diagnostic, not an equality gate.
        accelerated_colors_valid&=!compare_scalar||(quad_max==0&&scalar_full_max_error<=1);
        // Zero-weight skip compares the SAME variant and must match every RGBA
        // byte exactly. The pre-existing quad/scalar bound does not apply here.
        skip_colors_valid&=skip_byte_identity&&skip_max==0;
        fused_e_colors_valid&=!fused_e_diagnostic||(fused_e_error_max<=1&&fused_cpu_max<=1);
        require(sharpness<.1f||!full_compare||changed>50,"fused_sharpen_is_no_op");
        if(sharpness==0)require(pixels==complete,"fused_zero_strength_not_identity");
    }
    require(pair_colors_valid,"fused_two_pass_color_or_E_grid_mismatch");
    require(cpu_colors_valid,"fused_cpu_reference_mismatch");
    require(accelerated_colors_valid,"accelerated_changed_scalar_fused_pixels");
    require(skip_colors_valid,"zero_weight_skip_changed_legacy_fused_pixels");
    require(fused_e_colors_valid,"fused_E_quantization_or_independent_sharpen_mismatch");
}

std::array<surfxr::enhancement_reference::Image,2> gamma2_fixtures(unsigned sw,unsigned sh,const std::string& pattern){
    std::array<surfxr::enhancement_reference::Image,2> images;
    for(unsigned eye=0;eye<2;++eye){
        auto& fixture=images[eye];fixture=surfxr::enhancement_reference::fixture(sw,sh,eye);
        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw/8;++x){
            const auto offset=(std::size_t(y)*sw+x)*4;
            fixture.rgba[offset]=fixture.rgba[offset+1]=fixture.rgba[offset+2]=
                static_cast<std::uint8_t>(x*255/std::max(1u,sw/8-1));
        }
        std::uint32_t random=0x31415927u+eye;
        constexpr std::array<unsigned,10> codes{{0,1,10,11,63,64,127,128,254,255}};
        for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x){
            auto* p=fixture.rgba.data()+(std::size_t(y)*sw+x)*4;
            if(pattern=="nonsaturated"){
                for(unsigned c=0;c<3;++c){random^=random<<13;random^=random>>17;random^=random<<5;p[c]=std::uint8_t(96u+(random&63u));}
            }else if(pattern=="stress"){
                if(x<2||y<2||x+2>=sw||y+2>=sh)p[1]=((x/4+y/4+eye)&1u)?0:255;
                else if(y<sh/4&&x<sw/4)p[1]=((x/4+y/4+eye)&1u)?0:255;
                else if(y<sh/4&&x<sw/2)p[0]=p[1]=p[2]=std::uint8_t(codes[x%codes.size()]);
            }
            p[3]=255;
        }
    }
    return images;
}

Gamma2Difference gamma2_difference(const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b){
    require(a.size()==b.size(),"gamma2_comparison_size_mismatch");Gamma2Difference result;
    for(std::size_t i=0;i<a.size();++i){
        if(i%4==3){require(a[i]==255&&b[i]==255,"gamma2_nonopaque_output");continue;}
        result.add(a[i],b[i]);
    }
    return result;
}

std::vector<unsigned> gamma2_probe_axis(unsigned extent,bool full){
    std::vector<unsigned> values;
    if(full){for(unsigned i=0;i<extent;++i)values.push_back(i);return values;}
    for(unsigned i=0;i<=32;++i)values.push_back(i*(extent-1)/32);
    // Explicit clamped borders, quad helper edges and the two map joins.
    for(int center:{0,1,int(extent)-2,int(extent)-1,int(extent/4),int(3*extent/4)})
        for(int d=-1;d<=1;++d)values.push_back(unsigned(std::clamp(center+d,0,int(extent)-1)));
    std::sort(values.begin(),values.end());values.erase(std::unique(values.begin(),values.end()),values.end());return values;
}
}
