#pragma once
#include "surfxr/foveation_renderer.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"
#include "../xr/vulkan_fixture.hpp"
#include "../../../frame/media/v4l2_hevc.hpp"
#include "../../../frame/media/nv12_output.hpp"

namespace surfxr::nv12_test {
// Real Iris exports, production importer and production rendering barriers.
// Synthetic pixels only; keep two leases through later decoder outputs and
// decoder close, and compare GPU readback to the copied production path.
class DmaBufCheck {
    gpu_test::Vulkan& gpu_;
    Nv12FrameAllocator allocator_;
    std::unique_ptr<FoveationRenderer> renderer_;
    static constexpr unsigned w=512,h=512;
    struct Held {
        std::shared_ptr<Nv12FrameStorage> storage;
        std::shared_ptr<void> lease;
        std::uint64_t hash;
        std::vector<std::uint8_t> pixels;
        bool focus;
    };
    std::vector<Held> held_;
    std::uint64_t channels_=0,frames_=0;
    unsigned max_error_=0;
    static std::uint64_t hash(const Nv12FrameStorage& s){
        std::uint64_t value=1469598103934665603ull;
        for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?s.height/2:s.height);++y)
            for(unsigned x=0;x<s.width;++x){value^=s.data(p)[y*s.stride[p]+x];value*=1099511628211ull;}
        return value;
    }
    std::vector<std::uint8_t> render(Nv12FrameStorage& source,bool focus){
        auto* storage=dynamic_cast<VulkanNv12FrameStorage*>(&source);
        gpu_test::require(storage,"dmabuf_gpu_storage_type");
        std::array<FoveationEye,2> eyes{};
        for(unsigned eye=0;eye<2;++eye){auto& f=eyes[eye];
            f={storage->image(0),storage->view(0),gpu_.targets[eye].view,true};
            f.host_source=true;f.host_source_initialized=storage->initialized();f.foreign_source=storage->foreign();
            f.linear_layout=true;f.source_rect={eye*.5f,0,.5f,1};
            f.chroma_source=storage->image(1);f.chroma_view=storage->view(1);
            f.yuv_coefficients={.2126f,.0722f,16.f/255.f,255.f/219.f};
            if(focus){f.focus_source=f.source;f.focus_view=f.source_view;f.focus_host_source=true;
                f.focus_host_initialized=f.host_source_initialized;f.focus_source_rect={eye*.5f+.125f,.25f,.25f,.5f};
                f.focus_rect={128,128,256,256};}
        }
        gpu_test::require(renderer_->render(eyes,true),"dmabuf_render_skipped");
        storage->mark_initialized();renderer_->complete(false);return gpu_.read_pixels(w,h);
    }
    void compare(const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b){
        gpu_test::require(a.size()==b.size(),"dmabuf_readback_size");
        for(std::size_t i=0;i<a.size();++i){const auto error=unsigned(std::abs(int(a[i])-int(b[i])));
            max_error_=std::max(max_error_,error);++channels_;gpu_test::require(error<=1,"dmabuf_gpu_pixels_differ");}
    }
public:
    explicit DmaBufCheck(gpu_test::Vulkan& gpu):gpu_(gpu),allocator_(vulkan_nv12_frame_allocator(gpu.physical,gpu.device,true)){
        gpu_test::require(bool(allocator_.import),"dmabuf_importer_missing");
        for(auto& image:gpu.targets)gpu.image(image,w,h,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        gpu.buffer(gpu.readback,w*h*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        gpu.begin();for(auto& image:gpu.targets)gpu.barrier(image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);gpu.submit();
        renderer_=std::make_unique<FoveationRenderer>(gpu.physical,gpu.device,gpu.queue,gpu.family,gpu_test::kFormat,w,h);
    }
    void check(V4l2Hevc& decoder,const AVFrame& frame,Nv12FrameStorage& copied){
        validate_held();auto imported=decoder.import_capture(allocator_);
        gpu_test::require(bool(imported)&&valid_nv12_storage(*imported),"iris_dmabuf_import_failed");
        gpu_test::require(decoder.import_capture(allocator_)==imported,"iris_dmabuf_import_not_cached");
        auto lease=decoder.retain_capture();gpu_test::require(bool(lease),"iris_dmabuf_lease_failed");
        for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?imported->height/2:imported->height);++y)
            gpu_test::require(std::memcmp(imported->data(p)+y*imported->stride[p],frame.data[p]+y*frame.linesize[p],imported->width)==0,
                "iris_dmabuf_mapping_differ");
        decoder_detail::write_nv12(frame,copied);
        const bool focus=frames_%2!=0;
        auto expected=render(copied,focus);compare(expected,render(*imported,focus));
        if(held_.size()<2)held_.push_back({imported,lease,hash(*imported),std::move(expected),focus});
        ++frames_;
    }
    void validate_held(){for(const auto& f:held_)gpu_test::require(hash(*f.storage)==f.hash,"iris_leased_capture_overwritten");}
    void after_decoder_close(){validate_held();for(auto& f:held_)compare(f.pixels,render(*f.storage,f.focus));}
    void report()const{
        std::cout<<",\"gpu_import_tested\":true,\"gpu_pixel_channel_checks\":"<<channels_
            <<",\"gpu_max_code_error\":"<<max_error_<<",\"leased_capture_immutable\":true,\"import_survives_decoder_close\":true"
            <<",\"shared_eye_and_focus_planes_checked\":true,\"per_capture_import_cached\":true";
    }
};
}
