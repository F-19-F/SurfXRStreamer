#include "capture.hpp"
extern "C" {
#include <libavutil/opt.h>
}

namespace surfxr::transport::desktop {
namespace {void avcheck(int code,const char* error){require(code>=0,error);}}
Encoder::Encoder(Capture& capture,unsigned fps,unsigned bitrate){
    try{
        // Keep NVIDIA outputs on their GPU. An attached output on another
        // adapter uses one bounded staging surface into the NVIDIA encoder.
        const AVCodec* encoder=avcodec_find_encoder_by_name("hevc_nvenc");require(encoder,"desktop_nvenc_unavailable");
        encode_device_=capture.device();encode_context_=capture.context();
        Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> source;DXGI_ADAPTER_DESC description{};
        check(capture.device()->QueryInterface(IID_PPV_ARGS(&dxgi)),"desktop_encoder_source_device");check(dxgi->GetAdapter(&source),"desktop_encoder_source_adapter");check(source->GetDesc(&description),"desktop_encoder_source_description");
        if(description.VendorId!=0x10de){
            Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"desktop_encoder_factory");Com<IDXGIAdapter1> nvidia;
            for(unsigned index=0;;++index){Com<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(index,&candidate)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 d{};check(candidate->GetDesc1(&d),"desktop_encoder_adapter_description");if(d.VendorId==0x10de){nvidia=candidate;break;}}
            require(nvidia,"desktop_nvenc_adapter_unavailable");encode_context_.Reset();encode_device_.Reset();
            check(D3D11CreateDevice(nvidia.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&encode_device_,nullptr,&encode_context_),"desktop_cross_adapter_device");
            D3D11_TEXTURE2D_DESC staging{};capture.texture()->GetDesc(&staging);staging.BindFlags=0;staging.MiscFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            check(capture.device()->CreateTexture2D(&staging,nullptr,&staging_),"desktop_cross_adapter_staging");
        }
        device_=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);require(device_,"desktop_encoder_device_allocation");
        auto* hw=reinterpret_cast<AVHWDeviceContext*>(device_->data);auto* dx=reinterpret_cast<AVD3D11VADeviceContext*>(hw->hwctx);
        dx->device=encode_device_.Get();dx->device->AddRef();dx->device_context=encode_context_.Get();dx->device_context->AddRef();avcheck(av_hwdevice_ctx_init(device_),"desktop_encoder_device_init");
        frames_=av_hwframe_ctx_alloc(device_);require(frames_,"desktop_encoder_frames_allocation");
        auto* pool=reinterpret_cast<AVHWFramesContext*>(frames_->data);pool->format=AV_PIX_FMT_D3D11;pool->sw_format=AV_PIX_FMT_BGRA;pool->width=static_cast<int>(capture.display().encoded_width);pool->height=static_cast<int>(capture.display().encoded_height);pool->initial_pool_size=3;
        auto* textures=reinterpret_cast<AVD3D11VAFramesContext*>(pool->hwctx);textures->BindFlags=D3D11_BIND_RENDER_TARGET;
        avcheck(av_hwframe_ctx_init(frames_),"desktop_encoder_frames_init");
        codec_=avcodec_alloc_context3(encoder);require(codec_,"desktop_encoder_allocation");codec_->width=pool->width;codec_->height=pool->height;codec_->pix_fmt=AV_PIX_FMT_D3D11;codec_->hw_frames_ctx=av_buffer_ref(frames_);
        codec_->time_base={1,static_cast<int>(fps)};codec_->framerate={static_cast<int>(fps),1};codec_->bit_rate=static_cast<std::int64_t>(bitrate)*1000000;codec_->rc_max_rate=codec_->bit_rate;codec_->rc_buffer_size=static_cast<int>(codec_->bit_rate/fps*2);codec_->gop_size=static_cast<int>(fps);codec_->max_b_frames=0;
        // Desktop duplication preserves sRGB code values; encoding does not change their transfer curve.
        codec_->color_range=AVCOL_RANGE_MPEG;codec_->colorspace=AVCOL_SPC_BT709;codec_->color_primaries=AVCOL_PRI_BT709;codec_->color_trc=AVCOL_TRC_IEC61966_2_1;
        codec_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER|AV_CODEC_FLAG_LOW_DELAY;
        avcheck(av_opt_set(codec_->priv_data,"preset","p1",0),"desktop_encoder_preset");avcheck(av_opt_set(codec_->priv_data,"tune","ull",0),"desktop_encoder_tune");
        avcheck(av_opt_set(codec_->priv_data,"rc","cbr",0),"desktop_encoder_rc");avcheck(av_opt_set_int(codec_->priv_data,"zerolatency",1,0),"desktop_encoder_latency");avcheck(av_opt_set_int(codec_->priv_data,"forced-idr",1,0),"desktop_encoder_idr");
        avcheck(av_opt_set_int(codec_->priv_data,"delay",0,0),"desktop_encoder_delay");
        avcheck(avcodec_open2(codec_,encoder,nullptr),"desktop_nvenc_open_failed");
        frame_=av_frame_alloc();packet_=av_packet_alloc();require(frame_&&packet_,"desktop_encoder_packet_allocation");
    }catch(...){close();throw;}
}
Encoder::~Encoder(){close();}
void Encoder::close()noexcept{av_packet_free(&packet_);av_frame_free(&frame_);avcodec_free_context(&codec_);av_buffer_unref(&frames_);av_buffer_unref(&device_);}
Bytes Encoder::configuration()const{require(codec_->extradata_size>0,"desktop_encoder_missing_configuration");return Bytes(codec_->extradata,codec_->extradata+codec_->extradata_size);}
Encoded Encoder::encode(Capture& capture,bool idr,std::uint64_t timestamp){
    av_frame_unref(frame_);avcheck(av_hwframe_get_buffer(frames_,frame_,0),"desktop_encoder_frame");
    auto* texture=reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);const auto slice=static_cast<UINT>(reinterpret_cast<std::uintptr_t>(frame_->data[1]));
    if(staging_){
        capture.context()->CopyResource(staging_.Get(),capture.texture());D3D11_MAPPED_SUBRESOURCE mapped{};
        check(capture.context()->Map(staging_.Get(),0,D3D11_MAP_READ,0,&mapped),"desktop_cross_adapter_read");
        encode_context_->UpdateSubresource(texture,slice,nullptr,mapped.pData,mapped.RowPitch,0);capture.context()->Unmap(staging_.Get(),0);
    }else encode_context_->CopySubresourceRegion(texture,slice,0,0,0,capture.texture(),0,nullptr);
    frame_->pts=pts_++;frame_->pict_type=idr?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
    avcheck(avcodec_send_frame(codec_,frame_),"desktop_encoder_send");Encoded output;output.timestamp=timestamp;
    // zero-latency NVENC must return the corresponding picture; do not attach
    // a current timestamp to delayed output from a different source texture.
    const auto received=avcodec_receive_packet(codec_,packet_);avcheck(received,"desktop_encoder_delayed_output");
    require(packet_->pts==frame_->pts,"desktop_encoder_source_mismatch");require(packet_->size>0&&packet_->size<=8*1024*1024,"desktop_encoder_packet_bound");output.idr=(packet_->flags&AV_PKT_FLAG_KEY)!=0;output.bytes.assign(packet_->data,packet_->data+packet_->size);av_packet_unref(packet_);return output;
}
}
