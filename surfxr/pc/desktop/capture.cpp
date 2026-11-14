#include "capture.hpp"
#include <string>

namespace surfxr::transport::desktop {
namespace {
std::wstring name(HDESK desk){DWORD size=0;GetUserObjectInformationW(desk,UOI_NAME,nullptr,0,&size);require(size>=2&&size<=1024,"desktop_name_failed");std::wstring s(size/2,L'\0');require(GetUserObjectInformationW(desk,UOI_NAME,s.data(),size,&size),"desktop_name_failed");s.resize(wcslen(s.c_str()));return s;}
}
bool InputDesktop::changed()const{
    HDESK current=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);if(!current)return true;
    bool changed=true;try{changed=name(current)!=name_;}catch(...){}CloseDesktop(current);return changed;
}
bool InputDesktop::attach(){
    // SetThreadDesktop alone succeeds with narrow object rights, but input
    // injection also needs the generic write rights on the interactive desktop.
    HDESK current=OpenInputDesktop(0,FALSE,GENERIC_READ|GENERIC_WRITE|DESKTOP_SWITCHDESKTOP);
    require(current,"desktop_access_denied_service_required");std::wstring current_name;
    try{current_name=name(current);}catch(...){CloseDesktop(current);throw;}
    if(desktop_&&current_name==name_){CloseDesktop(current);return false;}
    if(!SetThreadDesktop(current)){CloseDesktop(current);fail("desktop_thread_attach_failed");}
    if(desktop_)CloseDesktop(desktop_);desktop_=current;name_=std::move(current_name);return true;
}
std::vector<unsigned> capture_monitors(unsigned preferred){
    Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"desktop_dxgi_factory");
    std::vector<unsigned> result;unsigned seen=0;
    for(unsigned a=0;;++a){Com<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        for(unsigned o=0;;++o){Com<IDXGIOutput> output;if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_OUTPUT_DESC d{};check(output->GetDesc(&d),"desktop_output_description");if(!d.AttachedToDesktop)continue;
            const auto id=seen++;const auto width=d.DesktopCoordinates.right-d.DesktopCoordinates.left,height=d.DesktopCoordinates.bottom-d.DesktopCoordinates.top;
            if(width>=32&&width<=4096&&height>=32&&height<=4096&&std::uint64_t{static_cast<unsigned>(width)}*height<=16*1024*1024){
                if(id==preferred)result.insert(result.begin(),id);else if(result.size()<4)result.push_back(id);
                if(result.size()>4)result.resize(4);
            }
        }
    }require(!result.empty(),"desktop_monitor_unavailable");return result;
}
Capture::Capture(unsigned monitor,bool synthetic,unsigned synthetic_width,unsigned synthetic_height){
    require((synthetic_width==0&&synthetic_height==0)||(synthetic&&synthetic_width&&synthetic_height),"desktop_synthetic_extent_requires_synthetic_capture");
    require(!synthetic_width||(synthetic_width>=32&&synthetic_width<=4096&&synthetic_height>=32&&synthetic_height<=4096),"desktop_synthetic_extent_unsupported");
    desktop_.attach();Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"desktop_dxgi_factory");
    unsigned seen=0;Com<IDXGIAdapter1> selected;Com<IDXGIOutput> output;
    for(unsigned adapter_index=0;!selected;++adapter_index){
        Com<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(adapter_index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        for(unsigned output_index=0;;++output_index){Com<IDXGIOutput> candidate;if(adapter->EnumOutputs(output_index,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_OUTPUT_DESC desc{};check(candidate->GetDesc(&desc),"desktop_output_description");if(!desc.AttachedToDesktop)continue;
            if(seen++==monitor){selected=adapter;output=candidate;break;}
        }
    }
    require(selected&&output,"desktop_monitor_unavailable");DXGI_OUTPUT_DESC desc{};check(output->GetDesc(&desc),"desktop_output_description");
    require(desc.Rotation>=DXGI_MODE_ROTATION_UNSPECIFIED&&desc.Rotation<=DXGI_MODE_ROTATION_ROTATE270,"desktop_rotation_unsupported");
    display_={desc.DesktopCoordinates.left,desc.DesktopCoordinates.top,desc.DesktopCoordinates.right-desc.DesktopCoordinates.left,desc.DesktopCoordinates.bottom-desc.DesktopCoordinates.top,0,0};
    if(synthetic_width){display_.width=static_cast<int>(synthetic_width);display_.height=static_cast<int>(synthetic_height);}
    display_.encoded_width=static_cast<unsigned>(display_.width)&~1u;display_.encoded_height=static_cast<unsigned>(display_.height)&~1u;
    display_.width=static_cast<int>(display_.encoded_width);display_.height=static_cast<int>(display_.encoded_height);
    require(display_.encoded_width>=32&&display_.encoded_width<=4096&&display_.encoded_height>=32&&display_.encoded_height<=4096&&std::uint64_t{display_.encoded_width}*display_.encoded_height<=16*1024*1024,"desktop_capture_resolution_unsupported");
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device_,nullptr,&context_),"desktop_d3d11_device");
    Com<IDXGIOutput1> output1;check(output.As(&output1),"desktop_dxgi_output1");
    if(!synthetic)check(output1->DuplicateOutput(device_.Get(),&duplication_),"desktop_duplication_access_denied_or_unavailable");
    D3D11_TEXTURE2D_DESC texture{};texture.Width=display_.encoded_width;texture.Height=display_.encoded_height;texture.MipLevels=1;texture.ArraySize=1;texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    check(device_->CreateTexture2D(&texture,nullptr,&image_),"desktop_capture_texture");
    check(device_->CreateTexture2D(&texture,nullptr,&background_),"desktop_background_texture");
    if(desc.Rotation>DXGI_MODE_ROTATION_IDENTITY)rotation_=std::make_unique<Rotation>(device_.Get(),context_.Get(),background_.Get(),desc.Rotation);
    cursor_=std::make_unique<Cursor>(device_.Get(),context_.Get(),image_.Get(),background_.Get(),display_);
    if(synthetic){Com<ID3D11RenderTargetView> target;check(device_->CreateRenderTargetView(background_.Get(),nullptr,&target),"desktop_synthetic_target");const float color[4]{.1f,.3f,.6f,1};context_->ClearRenderTargetView(target.Get(),color);context_->CopyResource(image_.Get(),background_.Get());}
}
Capture::~Capture()=default;
bool Capture::poll(){
    require(!desktop_.changed(),"desktop_changed");DXGI_OUTDUPL_FRAME_INFO info{};Com<IDXGIResource> resource;
    const auto result=duplication_->AcquireNextFrame(0,&info,&resource);if(result==DXGI_ERROR_WAIT_TIMEOUT)return false;
    check(result,"desktop_capture_access_lost");
    struct Release {IDXGIOutputDuplication* d;~Release(){d->ReleaseFrame();}} release{duplication_.Get()};
    Com<ID3D11Texture2D> source;check(resource.As(&source),"desktop_capture_resource");
    if(rotation_)rotation_->copy(source.Get());else {D3D11_BOX box{0,0,0,display_.encoded_width,display_.encoded_height,1};context_->CopySubresourceRegion(background_.Get(),0,0,0,0,source.Get(),0,&box);}return true;
}
void Capture::compose_cursor(){context_->CopyResource(image_.Get(),background_.Get());if(cursor_)cursor_->draw();}
}
