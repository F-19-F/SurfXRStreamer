// Synthetic offscreen checks of the actual FrameRender, CSOs and PoseHistory.
#include "platform/win32/FrameRender.h"
#include "surfxr_server/PoseHistory.h"
#include "surfxr_server/bindings.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

uint64_t g_DriverTestMode = 0;
extern "C" {
const unsigned char *FRAME_RENDER_VS_CSO_PTR=nullptr, *FRAME_RENDER_PS_CSO_PTR=nullptr;
const unsigned char *QUAD_SHADER_CSO_PTR=nullptr, *COMPRESS_AXIS_ALIGNED_CSO_PTR=nullptr;
const unsigned char *COLOR_CORRECTION_CSO_PTR=nullptr, *RGBTOYUV420_CSO_PTR=nullptr;
unsigned int FRAME_RENDER_VS_CSO_LEN=0, FRAME_RENDER_PS_CSO_LEN=0, QUAD_SHADER_CSO_LEN=0;
unsigned int COMPRESS_AXIS_ALIGNED_CSO_LEN=0, COLOR_CORRECTION_CSO_LEN=0, RGBTOYUV420_CSO_LEN=0;
FfiFoveationCenters GetEyeTrackedFoveationCenters(unsigned long long){return {};}
FfiFrameGazeCrops GetFrameGazeCrops(unsigned long long){return {};}
void ReportEncoderFoveationCenters(unsigned long long,float,float,float,float){}
}

namespace {
constexpr unsigned extent=256;
using Point=std::array<double,2>;
using Ray=std::array<double,3>;
const std::array<vr::HmdRect2_t,2> projections{{{{-.9f,-1.1f},{1.2f,.8f}},{{-1.2f,-1.1f},{.9f,.8f}}}};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void checked(HRESULT value,const char* reason){require(SUCCEEDED(value),reason);}
std::vector<unsigned char> shader(const char* name){
    std::ifstream input(std::filesystem::path(SURFXR_RENDER_SHADER_DIR)/name,std::ios::binary);
    require(bool(input),"pose_probe_shader_missing");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
FfiDeviceMotion motion(double angle,Ray axis){
    const auto scale=std::sin(angle/2)/std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
    FfiDeviceMotion m{};m.pose.orientation={float(axis[0]*scale),float(axis[1]*scale),float(axis[2]*scale),float(std::cos(angle/2))};
    return m;
}
vr::HmdMatrix34_t matrix(FfiDeviceMotion m){
    const auto q=m.pose.orientation;const double x=q.x,y=q.y,z=q.z,w=q.w;
    return {{{float(1-2*(y*y+z*z)),float(2*(x*y-z*w)),float(2*(x*z+y*w)),0},
             {float(2*(x*y+z*w)),float(1-2*(x*x+z*z)),float(2*(y*z-x*w)),0},
             {float(2*(x*z-y*w)),float(2*(y*z+x*w)),float(1-2*(x*x+y*y)),0}}};
}
Point project(const vr::HmdMatrix34_t& pose,const Ray& world,unsigned eye){
    Ray view{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)view[i]+=pose.m[j][i]*world[j];
    const auto& p=projections[eye];
    return {extent*(view[0]/-view[2]-p.vTopLeft.v[0])/(p.vBottomRight.v[0]-p.vTopLeft.v[0]),
            extent*(p.vBottomRight.v[1]-view[1]/-view[2])/(p.vBottomRight.v[1]-p.vTopLeft.v[1])};
}
ComPtr<ID3D11Texture2D> marker(ID3D11Device* device,Point center,unsigned channel,bool overlay){
    std::vector<unsigned char> pixels(extent*extent*4);
    for(unsigned y=0;y<extent;++y)for(unsigned x=0;x<extent;++x){
        const double dx=x+.5-center[0],dy=y+.5-center[1];
        const auto value=static_cast<unsigned char>(std::lround(255*std::exp(-(dx*dx+dy*dy)/8)));
        pixels[(y*extent+x)*4+channel]=overlay?255:value;
        pixels[(y*extent+x)*4+3]=overlay?value:255;
    }
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=extent;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),extent*4,0};ComPtr<ID3D11Texture2D> result;
    checked(device->CreateTexture2D(&desc,&data,&result),"pose_probe_source");return result;
}
double distance(Point a,Point b){return std::hypot(a[0]-b[0],a[1]-b[1]);}
}

void frame_render_pose_probe(Settings& settings){
    auto vs=shader("FrameRenderVS.cso"),ps=shader("FrameRenderPS.cso"),quad=shader("QuadVertexShader.cso");
    FRAME_RENDER_VS_CSO_PTR=vs.data();FRAME_RENDER_VS_CSO_LEN=unsigned(vs.size());
    FRAME_RENDER_PS_CSO_PTR=ps.data();FRAME_RENDER_PS_CSO_LEN=unsigned(ps.size());
    QUAD_SHADER_CSO_PTR=quad.data();QUAD_SHADER_CSO_LEN=unsigned(quad.size());
    settings={};settings.m_renderWidth=extent*2;settings.m_renderHeight=extent;settings.m_encodingGamma=1;
    auto device=std::make_shared<CD3DRender>();require(device->Initialize(0),"pose_probe_device");
    int32_t adapter_index=0;std::wstring adapter;device->GetAdapterInfo(&adapter_index,adapter);
    FrameRender render(device);require(render.Startup(),"pose_probe_startup");
    auto left=matrix(motion(0,{0,1,0})),right=left;
    left.m[0][3]=-.032f;right.m[0][3]=.032f;
    render.SetViewParams(projections[0],left,projections[1],right);
    D3D11_TEXTURE2D_DESC desc{};render.GetTexture()->GetDesc(&desc);
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;checked(device->GetDevice()->CreateTexture2D(&desc,nullptr,&staging),"pose_probe_staging");
    const Ray scene_ray{-.25,.15,-1},overlay_ray{.2,-.12,-1};
    double max_corrected=0,max_old=0,max_timed=0;unsigned cases=0;
    for(const auto axis:{Ray{0,1,0},Ray{1,0,0},Ray{0,0,1},Ray{1,2,3}}){
        for(const double direction:{-1.,1.}){
            const auto target_motion=motion(direction*.04,axis);
            PoseHistory history;history.OnPoseUpdated(1'000'000'000,target_motion);
            history.OnPoseUpdated(1'008'333'333,motion(direction*.08,axis));
            vr::HmdMatrix34_t poses[]{matrix(motion(direction*.052,axis)),matrix(motion(-direction*.025,axis))};
            const auto match=history.GetBestPoseMatch(poses[0]);
            require(match&&match->targetTimestampNs==1'000'000'000,"pose_probe_history_choice");
            auto timed_motion=target_motion;timed_motion.poseEpoch=1;
            history.OnPoseUpdated(1'010'000'000,timed_motion);
            const auto timed=history.GetFramePose(poses[0]);
            require(timed&&timed->targetTimestampNs==1'010'000'000,"pose_probe_timed_identity");
            ComPtr<ID3D11Texture2D> textures[2][2];ID3D11Texture2D* input[2][2];
            vr::VRTextureBounds_t bounds[2][2];
            for(unsigned eye=0;eye<2;++eye)for(unsigned layer=0;layer<2;++layer){
                textures[layer][eye]=marker(device->GetDevice(),project(poses[layer],layer?overlay_ray:scene_ray,eye),layer?0:1,layer!=0);
                input[layer][eye]=textures[layer][eye].Get();bounds[layer][eye]={0,0,1,1};
            }
            // Negative control reproduces the former layer-zero target. Both
            // controls retain the same matched metadata identity on the client.
            for(unsigned mode:{0u,1u,2u}){
                const auto target=mode==2?timed->poseMatrix:(mode==0?poses[0]:match->poseMatrix);
                const auto metadata=mode==2?timed->poseMatrix:match->poseMatrix;
                require(render.RenderFrame(input,bounds,poses,target,2,false,
                    match->targetTimestampNs,"",""),"pose_probe_render");
                device->GetContext()->CopyResource(staging.Get(),render.GetTexture().Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(device->GetContext()->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"pose_probe_readback");
                std::array<std::array<Point,2>,2> centers{};
                for(unsigned eye=0;eye<2;++eye)for(unsigned layer=0;layer<2;++layer){
                    double weight=0;Point total{};const unsigned channel=layer?0:1;
                    // Exclude the exposed border: no depth/extra source pixels.
                    for(unsigned y=24;y<extent-24;++y)for(unsigned x=24;x<extent-24;++x){
                        const auto* pixel=static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch+(eye*extent+x)*4;
                        const double value=pixel[channel];weight+=value;total[0]+=value*(x+.5);total[1]+=value*(y+.5);
                    }
                    require(weight>100,"pose_probe_missing_marker");centers[eye][layer]={total[0]/weight,total[1]/weight};
                }
                device->GetContext()->Unmap(staging.Get(),0);
                for(unsigned eye=0;eye<2;++eye)for(unsigned layer=0;layer<2;++layer){
                    const auto error=distance(centers[eye][layer],project(metadata,layer?overlay_ray:scene_ray,eye));
                    if(mode==0)max_old=std::max(max_old,error);
                    else {if(mode==1)max_corrected=std::max(max_corrected,error);else max_timed=std::max(max_timed,error);require(error<.18,"pose_probe_image_metadata_disagree");}
                }
            }
            ++cases;
        }
    }
    require(max_old>1,"pose_probe_negative_control_not_sensitive");
    std::cout<<"{\"cases\":"<<cases<<",\"eyes\":2,\"layers\":2,\"old_target_max_error_pixels\":"<<max_old
        <<",\"matched_target_max_error_pixels\":"<<max_corrected<<",\"submitted_target_max_error_pixels\":"<<max_timed<<",\"adapter\":\""<<std::filesystem::path(adapter).string()
        <<"\",\"production_renderer\":true,\"production_pose_history\":true,\"network_used\":false,\"passed\":true}\n";
}
