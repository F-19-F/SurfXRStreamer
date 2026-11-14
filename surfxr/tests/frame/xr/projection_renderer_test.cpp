#include "projection_renderer_test_cases.hpp"
using namespace surfxr::tests::projection;

int main(int argc,char** argv){
    try{
        if(argc==2&&std::strcmp(argv[1],"--self-test")==0){const auto groups=cpu_tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"vulkan_device_created\":false,\"network_used\":false,\"openxr_session_started\":false}\n";return 0;}
        if(argc==2&&std::strcmp(argv[1],"--gpu-quality-test")==0){gpu_quality_test();return 0;}
        if(argc==2&&std::strcmp(argv[1],"--gpu-nv12-test")==0){gpu_nv12_test();return 0;}
        if(argc>=2&&std::strcmp(argv[1],"--gpu-benchmark")==0){
            unsigned size=4608,frames=60;bool dirty=true,timing=false;auto sampling=surfxr::ProjectionSampling::QualitySS;
            std::array<bool,5> seen{};
            for(int i=2;i<argc;i+=2){require(i+1<argc,"benchmark_option_requires_value");const std::string key=argv[i],value=argv[i+1];
                if(key=="--size"||key=="--frames"){const unsigned index=key=="--size"?0:1;require(!seen[index]&&!value.empty(),"invalid_benchmark_numeric_option");seen[index]=true;
                    unsigned number=0;for(char c:value){require(c>='0'&&c<='9'&&number<=6144,"invalid_benchmark_number");number=number*10+static_cast<unsigned>(c-'0');}
                    if(index==0){require(number>=512&&number<=6144,"benchmark_size_must_be_512_to_6144");size=number;}
                    else{require(number>=10&&number<=300,"benchmark_frames_must_be_10_to_300");frames=number;}
                }else if(key=="--sampling"){require(!seen[2],"duplicate_benchmark_sampling");seen[2]=true;
                    if(value=="baseline")sampling=surfxr::ProjectionSampling::Baseline;else if(value=="quality")sampling=surfxr::ProjectionSampling::QualitySS;else throw std::runtime_error("benchmark_sampling_must_be_baseline_or_quality");
                }else if(key=="--updates"){require(!seen[3],"duplicate_benchmark_updates");seen[3]=true;
                    if(value=="on")dirty=true;else if(value=="off")dirty=false;else throw std::runtime_error("benchmark_updates_must_be_on_or_off");
                }else if(key=="--timing"){require(!seen[4],"duplicate_benchmark_timing");seen[4]=true;
                    if(value=="on")timing=true;else if(value=="off")timing=false;else throw std::runtime_error("benchmark_timing_must_be_on_or_off");
                }else throw std::runtime_error("unknown_benchmark_option");
            }
            gpu_benchmark(size,sampling,dirty,frames,timing);return 0;
        }
        if(argc>=3&&std::strcmp(argv[1],"--gpu-preview")==0){
            unsigned size=kDefaultTarget,samples=4;bool size_seen=false,samples_seen=false,sampling_seen=false;auto sampling=surfxr::ProjectionSampling::Baseline;
            for(int i=3;i<argc;i+=2){require(i+1<argc,"preview_option_requires_value");
                if(std::strcmp(argv[i],"--size")==0){require(!size_seen,"duplicate_preview_size");size_seen=true;require(argv[i+1][0]!=0,"preview_size_must_be_512_to_6144");size=0;
                    for(const char* p=argv[i+1];*p;++p){require(*p>='0'&&*p<='9'&&size<=614,"preview_size_must_be_512_to_6144");size=size*10+static_cast<unsigned>(*p-'0');}
                    require(size>=512&&size<=6144,"preview_size_must_be_512_to_6144");
                }else if(std::strcmp(argv[i],"--samples")==0){require(!samples_seen,"duplicate_preview_samples");samples_seen=true;
                    const auto* value=argv[i+1];require(value[0]!=0&&value[1]==0&&(value[0]=='1'||value[0]=='2'||value[0]=='4'),"preview_samples_must_be_1_2_or_4");samples=static_cast<unsigned>(value[0]-'0');
                }else if(std::strcmp(argv[i],"--sampling")==0){require(!sampling_seen,"duplicate_preview_sampling");sampling_seen=true;
                    if(std::strcmp(argv[i+1],"baseline")==0)sampling=surfxr::ProjectionSampling::Baseline;
                    else if(std::strcmp(argv[i+1],"quality")==0)sampling=surfxr::ProjectionSampling::QualitySS;
                    else throw std::runtime_error("preview_sampling_must_be_baseline_or_quality");
                }else throw std::runtime_error("unknown_preview_option");
            }
            gpu_preview(argv[2],size,samples,sampling);return 0;
        }
        std::cout<<"surfxr-projection-test --self-test | --gpu-quality-test | --gpu-nv12-test | --gpu-benchmark [--size 512..6144] [--frames 10..300] [--sampling baseline|quality] [--updates on|off] [--timing on|off] | --gpu-preview NEW_FILE.ppm [--size 512..6144] [--samples 1|2|4] [--sampling baseline|quality]\n";return argc==1||(argc==2&&std::strcmp(argv[1],"--help")==0)?0:1;
    }catch(const std::exception& e){std::cerr<<"projection_test: "<<e.what()<<'\n';return 1;}
}
