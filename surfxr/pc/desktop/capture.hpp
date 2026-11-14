#pragma once
#include "windows.hpp"
#include <d3d11.h>
#include <dxgi1_5.h>
#include "cursor.hpp"
#include "rotation.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace surfxr::transport::desktop {
// Existing attached outputs only; the preferred output stays panel zero.
std::vector<unsigned> capture_monitors(unsigned preferred);
class Capture {
public:
    explicit Capture(unsigned monitor,bool synthetic=false,unsigned synthetic_width=0,unsigned synthetic_height=0);
    ~Capture();
    // A timeout leaves the owned last texture valid only on the same desktop.
    bool poll();
    void compose_cursor();
    ID3D11Device* device()const{return device_.Get();}
    ID3D11DeviceContext* context()const{return context_.Get();}
    ID3D11Texture2D* texture()const{return image_.Get();}
    const Display& display()const{return display_;}
private:
    InputDesktop desktop_;Display display_;
    Com<ID3D11Device> device_;Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutputDuplication> duplication_;Com<ID3D11Texture2D> image_,background_;
    std::unique_ptr<Cursor> cursor_;
    std::unique_ptr<Rotation> rotation_;
};
struct Encoded {Bytes bytes;bool idr=false;std::uint64_t timestamp=0;};
class Encoder {
public:
    Encoder(Capture&,unsigned fps,unsigned bitrate);
    ~Encoder();
    Bytes configuration()const;
    Encoded encode(Capture&,bool idr,std::uint64_t timestamp);
private:
    AVCodecContext* codec_=nullptr;AVBufferRef *device_=nullptr,*frames_=nullptr;
    AVFrame* frame_=nullptr;AVPacket* packet_=nullptr;std::int64_t pts_=0;
    Com<ID3D11Device> encode_device_;Com<ID3D11DeviceContext> encode_context_;
    Com<ID3D11Texture2D> staging_;
    void close()noexcept;
};
}
