#include "surfxr/ports.hpp"
#include "surfxr_discovery.hpp"
using namespace surfxr::discovery_detail;
int main(int argc,char** argv){
    const char* stage="arguments";
    try {
        if(argc==2&&std::strcmp(argv[1],"--help")==0){
            std::cout<<"SurfXR discovery: --scan|--host IPV4 --local-address IPV4 --prefix N [--surfxr-control-port N]\n";return 0;
        }
        int first=1;
        if(argc>2&&std::strcmp(argv[1],"--backend")==0){require(std::strcmp(argv[2],"surfxr")==0,"invalid_backend");first=3;}
            std::string host,local;unsigned prefix=0,port=surfxr::ports::vr_control;bool broadcast=false;
            for(int i=first;i<argc;++i){
                const std::string option=argv[i];if(option=="--scan"){broadcast=true;continue;}
                require(i+1<argc,"invalid_surfxr_scan_arguments");const char* value=argv[++i];
                if(option=="--host")host=value;
                else if(option=="--local-address")local=value;
                else if(option=="--prefix"||option=="--surfxr-control-port"){
                    unsigned n=0;const auto end=value+std::strlen(value);const auto parsed=std::from_chars(value,end,n);
                    require(parsed.ec==std::errc{}&&parsed.ptr==end,"invalid_surfxr_scan_number");
                    if(option=="--prefix")prefix=n;else port=n;
                }else throw ProbeError("invalid_surfxr_scan_arguments");
            }
            require(!local.empty()&&(broadcast!=!host.empty()),"invalid_surfxr_scan_arguments");
            std::cout<<scan_surfxr(local.c_str(),prefix,port,host,stage)<<'\n';return 0;

    }catch(const std::exception& error){
        std::cerr<<"{\"status\":\"probe_error\",\"stage\":\""<<stage<<"\",\"connection_started\":false}\n";return 1;
    }
}
