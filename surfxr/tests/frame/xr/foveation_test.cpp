#include "surfxr/foveation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool near(float a,float b,float tolerance=1e-6f){return std::abs(a-b)<=tolerance;}
void tests(){
    const std::array<float,5> original{{0,.25f,.5f,.75f,1}},encoded{{0,.125f,.5f,.875f,1}};
    for(unsigned i=0;i<5;++i){require(near(surfxr::foveation_encoded_uv(original[i]),encoded[i]),"inverse landmark");
        require(near(surfxr::foveation_original_uv(encoded[i]),original[i]),"forward landmark");}
    float previous=-1;
    for(unsigned i=0;i<=100000;++i){const float u=static_cast<float>(i)/100000;
        const float e=surfxr::foveation_encoded_uv(u);
        require(e>=0&&e<=1&&e>=previous,"monotonic range");previous=e;
        // Squaring near u=1 can round e to exactly 1 in float32. The inverse
        // square root magnifies this by at most sqrt(0.5 ULP / 2) < 0.00013.
        require(near(surfxr::foveation_original_uv(e),u,1.3e-4f),"inverse roundtrip");
        require(near(surfxr::foveation_encoded_uv(surfxr::foveation_original_uv(u)),u),"forward roundtrip");
        require(near(e,1-surfxr::foveation_encoded_uv(1-u)),"axis symmetry");}
    for(float edge:{.25f,.75f}){const float lo=surfxr::foveation_encoded_uv(edge-1e-5f),hi=surfxr::foveation_encoded_uv(edge+1e-5f);
        require(hi>=lo&&hi-lo<4e-5f,"continuous inverse boundary");}
    for(float edge:{.125f,.875f}){const float lo=surfxr::foveation_original_uv(edge-1e-5f),hi=surfxr::foveation_original_uv(edge+1e-5f);
        require(hi>=lo&&hi-lo<4e-5f,"continuous forward boundary");}
    // Asymmetric left/right and top/bottom markers would fail an axis swap or
    // vertical flip. Numeric goldens are independent of the helper formula.
    const std::array<std::array<float,2>,4> points{{{{.1f,.9f}},{{.3f,.2f}},{{.8f,.6f}},{{.45f,.7f}}}};
    const std::array<std::array<float,2>,4> goldens{{{{.02f,.98f}},{{.2f,.08f}},{{.92f,.65f}},{{.425f,.8f}}}};
    for(unsigned i=0;i<points.size();++i){const auto e=surfxr::foveation_encoded_uv(points[i]);
        require(near(e[0],goldens[i][0])&&near(e[1],goldens[i][1]),"asymmetric marker coordinate");
        require(near(surfxr::foveation_original_uv(e[0]),points[i][0])&&near(surfxr::foveation_original_uv(e[1]),points[i][1]),"asymmetric marker reconstruction");}
    // Simulated encode/decode of a non-symmetric analytic RGB test image,
    // sampled over both axes; preserves independent row/column gradients.
    for(unsigned y=0;y<=100;++y)for(unsigned x=0;x<=100;++x){const float u=x/100.f,v=y/100.f;
        const auto e=surfxr::foveation_encoded_uv(std::array<float,2>{u,v});
        const float sx=surfxr::foveation_original_uv(e[0]),sy=surfxr::foveation_original_uv(e[1]);
        require(near(sx,u,1e-5f)&&near(sy,v,1e-5f)&&near(.17f*sx+.73f*sy,.17f*u+.73f*v,1e-5f),"two-axis image roundtrip");}
}
}
int main(int argc,char** argv){
    if(argc!=2||std::string(argv[1])!="--self-test"){std::cerr<<"Usage: surfxr-foveation-test --self-test\n";return 1;}
    try{tests();std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"openxr_session_started\":false,\"gpu_used\":false,\"scenario_groups\":6}\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
