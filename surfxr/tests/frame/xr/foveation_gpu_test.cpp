#include "foveation_gpu_test_cases.hpp"
using namespace surfxr::tests::foveation_gpu;

int main(int argc,char** argv){
    try{
        if(argc==2&&std::strcmp(argv[1],"--timewarp-check")==0){run_timewarp();return 0;}
        if(argc==2&&std::strcmp(argv[1],"--gaze-atlas-check")==0){run_gaze_atlas();return 0;}
        if(argc==2&&std::strcmp(argv[1],"--linear-layout-check")==0){run(512,512,768,768,true);run_nv12();run_nv12(false);return 0;}
        if(argc==2&&std::strcmp(argv[1],"--focus-check")==0){run_focus(false);run_focus(true);return 0;}
        if(argc==2&&std::strcmp(argv[1],"--help")==0)std::cout<<"surfxr-foveation-gpu-test --focus-check|--linear-layout-check|--gaze-atlas-check|--timewarp-check\n";
        if(argc==1||(argc==2&&std::strcmp(argv[1],"--help")==0)){std::cout<<"surfxr-foveation-gpu-test --run [--source-width N --source-height N --width N --height N]\nvd-foveation-gpu-test --sharpen-check|--gamma2-check [--source-width N --source-height N --width N --height N --sharpness 0..1 --frames 1..200 --quad 0|1 --zero-weight-skip 0|1 --pattern mixed|stress|nonsaturated]\nvd-foveation-gpu-test --linear-source-probe [--source-width N --source-height N --width N --height N --frames 1..200]\n";return 0;}
        const bool linear_probe=std::strcmp(argv[1],"--linear-source-probe")==0;
        const bool gamma2=std::strcmp(argv[1],"--gamma2-check")==0;
        const bool sharpen=gamma2||std::strcmp(argv[1],"--sharpen-check")==0;
        require(linear_probe||sharpen||std::strcmp(argv[1],"--run")==0,"explicit_run_required");
        unsigned sw=linear_probe?2048:sharpen?96:512,sh=linear_probe?2176:sharpen?64:512,width=linear_probe?4608:sharpen?128:768,height=linear_probe?4896:sharpen?96:768;
        unsigned seen=0,frames=linear_probe?16:12;float sharpness=.35f;bool quad=false,zero_weight_skip=true;std::string pattern="mixed";
        for(int i=2;i<argc;i+=2){require(i+1<argc,"missing_option_value");unsigned bit=0;unsigned* out=nullptr;
            if(std::strcmp(argv[i],"--source-width")==0){bit=1;out=&sw;}else if(std::strcmp(argv[i],"--source-height")==0){bit=2;out=&sh;}
            else if(std::strcmp(argv[i],"--width")==0){bit=4;out=&width;}else if(std::strcmp(argv[i],"--height")==0){bit=8;out=&height;}
            else if(sharpen&&std::strcmp(argv[i],"--sharpness")==0){
                require(!(seen&16),"duplicate_option");seen|=16;std::size_t consumed=0;
                sharpness=std::stof(argv[i+1],&consumed);require(consumed==std::strlen(argv[i+1])&&std::isfinite(sharpness)&&sharpness>=0&&sharpness<=1,"invalid_sharpness");continue;
            }else if((sharpen||linear_probe)&&std::strcmp(argv[i],"--frames")==0){
                require(!(seen&32),"duplicate_option");seen|=32;frames=parse_size(argv[i+1],1);require(frames<=200,"invalid_frame_count");continue;
            }else if(sharpen&&std::strcmp(argv[i],"--quad")==0){
                require(!(seen&64),"duplicate_option");seen|=64;
                require(std::strcmp(argv[i+1],"0")==0||std::strcmp(argv[i+1],"1")==0,"invalid_quad_option");quad=std::strcmp(argv[i+1],"1")==0;continue;
            }else if(sharpen&&std::strcmp(argv[i],"--zero-weight-skip")==0){
                require(!(seen&128),"duplicate_option");seen|=128;
                require(std::strcmp(argv[i+1],"0")==0||std::strcmp(argv[i+1],"1")==0,"invalid_zero_weight_skip_option");
                zero_weight_skip=std::strcmp(argv[i+1],"1")==0;continue;
            }else if(sharpen&&std::strcmp(argv[i],"--pattern")==0){
                require(!(seen&256),"duplicate_option");seen|=256;pattern=argv[i+1];
                require(pattern=="mixed"||pattern=="stress"||pattern=="nonsaturated","invalid_fixture_pattern");continue;
            }
            else throw std::runtime_error("unknown_option");
            require(!(seen&bit),"duplicate_option");seen|=bit;*out=parse_size(argv[i+1],sharpen||linear_probe?16:256);
        }
        require(std::uint64_t{sw}*sh<=16'777'216&&std::uint64_t{width}*height<=33'554'432,"test_image_area_exceeds_policy");
        if(linear_probe)run_linear_source_probe(sw,sh,width,height,frames);
        else if(gamma2)run_gamma2(sw,sh,width,height,sharpness,frames,quad,zero_weight_skip,pattern);
        else if(sharpen)run_sharpen(sw,sh,width,height,sharpness,frames,quad,zero_weight_skip,pattern);
        else run(sw,sh,width,height);
        return 0;
    }catch(const std::exception& e){std::cerr<<"foveation_gpu_test: "<<e.what()<<'\n';return 1;}
}
