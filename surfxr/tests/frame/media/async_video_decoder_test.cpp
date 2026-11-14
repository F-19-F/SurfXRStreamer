#include "surfxr/async_video_decoder.hpp"
#include <cstring>
#include <iostream>

int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cerr<<"Use --self-test\n";return 1;}
    try{
        surfxr::async_video_decoder_self_test();
        std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"hardware_devices_used\":false}\n";return 0;
    }catch(const std::exception& error){
        std::cerr<<"Async decoder self-test failed: "<<error.what()<<'\n';return 1;
    }
}
