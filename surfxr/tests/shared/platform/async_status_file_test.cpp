#include "surfxr/async_status_file.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Predicate> void wait_for(Predicate predicate,const char* message){
    const auto deadline=Clock::now()+std::chrono::seconds(3);
    while(!predicate()){
        require(Clock::now()<deadline,message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
std::string read(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}

unsigned tests(){
    unsigned groups=0;
    {
        std::mutex mutex;std::condition_variable changed;
        bool entered=false,released=false;
        std::vector<std::string> written;
        {
            surfxr::AsyncStatusFile writer(surfxr::AsyncStatusFile::Writer{[&](const std::string& snapshot){
                std::unique_lock<std::mutex> lock(mutex);
                if(snapshot=="first"){
                    entered=true;changed.notify_one();
                    if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;}))return false;
                }
                written.push_back(snapshot);return true;
            }});
            writer.publish("first");
            {
                std::unique_lock<std::mutex> lock(mutex);
                require(changed.wait_for(lock,std::chrono::seconds(3),[&]{return entered;}),"writer_did_not_start");
            }
            const auto begin=Clock::now();
            for(unsigned i=0;i<1000;++i)writer.publish(std::to_string(i));
            writer.publish("latest");
            const auto duration=Clock::now()-begin;
            {
                std::lock_guard<std::mutex> lock(mutex);
                require(written.empty(),"blocked_writer_did_not_remain_blocked");
                released=true;
            }
            changed.notify_one();
            require(duration<std::chrono::milliseconds(250),"publishing_waited_for_blocked_writer");
            // Destruction must finish the current write and the one latest
            // pending snapshot, even when the worker has not taken it yet.
        }
        require(written==std::vector<std::string>{"first","latest"},"pending_snapshots_not_coalesced_or_shutdown_lost_latest");
    }++groups;
    {
        std::atomic<unsigned> calls{0};
        surfxr::AsyncStatusFile writer(surfxr::AsyncStatusFile::Writer{[&](const std::string& value){
            calls.fetch_add(1,std::memory_order_relaxed);
            if(value=="throw")throw std::runtime_error("synthetic_writer_failure");
            return value!="false";
        }});
        writer.publish("throw");wait_for([&]{return writer.errors()==1;},"writer_exception_not_counted");
        writer.publish("false");wait_for([&]{return writer.errors()==2;},"writer_failure_not_counted");
        writer.publish(std::string(surfxr::AsyncStatusFile::maximum_snapshot_bytes+1,'x'));
        require(writer.errors()==3&&calls.load(std::memory_order_relaxed)==2,"oversized_snapshot_not_rejected");
        writer.publish("recovered");wait_for([&]{return calls.load(std::memory_order_relaxed)==3;},"writer_did_not_recover");
    }++groups;
    {
        surfxr::AsyncStatusFile disabled(std::string{});
        disabled.publish("ignored");require(disabled.errors()==0,"disabled_writer_not_noop");
    }++groups;
    {
        const auto unique=std::to_string(Clock::now().time_since_epoch().count());
        const auto path=std::filesystem::temp_directory_path()/("surfxr-async-status-test-"+unique+".json");
        struct Cleanup{
            std::filesystem::path path;
            ~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}
        } cleanup{path};
        surfxr::AsyncStatusFile writer(path.string());
        writer.publish("{\"value\":1}\n");
        wait_for([&]{return read(path)=="{\"value\":1}\n";},"initial_atomic_status_not_written");
#ifdef _WIN32
        HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(held!=INVALID_HANDLE_VALUE,"status_reader_not_opened");
        struct HeldFile{HANDLE file;~HeldFile(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}} held_file{held};
        writer.publish("{\"value\":2}\n");
        wait_for([&]{return writer.errors()>0;},"held_reader_failure_not_counted");
        require(read(path)=="{\"value\":1}\n","failed_replace_modified_previous_snapshot");
        CloseHandle(held);held_file.file=INVALID_HANDLE_VALUE;
#else
        struct stat mode{};require(stat(path.c_str(),&mode)==0&&(mode.st_mode&0777)==0600,"status_permissions_not_private");
#endif
        writer.publish("{\"value\":3}\n");
        wait_for([&]{return read(path)=="{\"value\":3}\n";},"atomic_status_replace_did_not_recover");
    }++groups;
    return groups;
}
}

int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cout<<"surfxr-async-status-test --self-test\n";return argc==1?0:1;}
    try{const auto groups=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"network_used\":false}\n";return 0;}
    catch(const std::exception& error){std::cerr<<"async_status_test: "<<error.what()<<'\n';return 1;}
}
