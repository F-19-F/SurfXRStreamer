#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace surfxr {

// Optional telemetry must not put filesystem latency on the transport thread.
// There is one active write and at most one pending snapshot; newer snapshots
// replace pending ones. Destruction drains the latest snapshot at shutdown.
class AsyncStatusFile {
public:
    using Writer=std::function<bool(const std::string&)>;
    static constexpr std::size_t maximum_snapshot_bytes=64u*1024u;

    explicit AsyncStatusFile(std::string path):path_(std::move(path)){
        if(path_.empty())return;
        try{
#ifdef _WIN32
            const auto process=GetCurrentProcessId();
#else
            const auto process=getpid();
#endif
            temporary_=path_;
            temporary_+=".tmp."+std::to_string(process)+"."+
                std::to_string(next_id_.fetch_add(1,std::memory_order_relaxed));
            writer_=[this](const std::string& snapshot){return write_snapshot(snapshot);};
            start();
        }catch(...){errors_.fetch_add(1,std::memory_order_relaxed);}
    }
    // Injection permits a deliberately blocked/failing writer in concurrency
    // tests without depending on disk speed or modifying a real status file.
    explicit AsyncStatusFile(Writer writer):writer_(std::move(writer)){if(writer_)start();}
    AsyncStatusFile(const AsyncStatusFile&)=delete;
    AsyncStatusFile& operator=(const AsyncStatusFile&)=delete;

    ~AsyncStatusFile(){
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_=true;
        }
        changed_.notify_one();
        if(worker_.joinable())worker_.join();
    }

    void publish(std::string snapshot){
        if(!enabled_)return;
        if(snapshot.size()>maximum_snapshot_bytes){errors_.fetch_add(1,std::memory_order_relaxed);return;}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_)return;
            pending_=std::move(snapshot);
            have_pending_=true;
        }
        changed_.notify_one();
    }

    std::uint64_t errors()const noexcept{return errors_.load(std::memory_order_relaxed);}

private:
    void start()noexcept{
        try{worker_=std::thread([this]{run();});enabled_=true;}
        catch(...){errors_.fetch_add(1,std::memory_order_relaxed);}
    }

    bool write_snapshot(const std::string& snapshot)noexcept{
#ifdef _WIN32
        HANDLE file=CreateFileW(temporary_.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return false;
        std::size_t offset=0;
        bool success=true;
        while(offset<snapshot.size()){
            DWORD written=0;
            if(!WriteFile(file,snapshot.data()+offset,static_cast<DWORD>(snapshot.size()-offset),&written,nullptr)||!written){
                success=false;break;
            }
            offset+=written;
        }
        if(!CloseHandle(file))success=false;
        if(success&&MoveFileExW(temporary_.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING))return true;
        DeleteFileW(temporary_.c_str());
        return false;
#else
        const int file=open(temporary_.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        if(file<0)return false;
        bool success=fchmod(file,0600)==0;
        std::size_t offset=0;
        while(success&&offset<snapshot.size()){
            const auto written=write(file,snapshot.data()+offset,snapshot.size()-offset);
            if(written<0&&errno==EINTR)continue;
            if(written<=0){success=false;break;}
            offset+=static_cast<std::size_t>(written);
        }
        if(close(file)!=0)success=false;
        if(success&&rename(temporary_.c_str(),path_.c_str())==0)return true;
        unlink(temporary_.c_str());
        return false;
#endif
    }

    void run()noexcept{
        for(;;){
            std::string snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock,[&]{return stopping_||have_pending_;});
                if(!have_pending_)return;
                snapshot=std::move(pending_);
                have_pending_=false;
            }
            try{if(!writer_(snapshot))errors_.fetch_add(1,std::memory_order_relaxed);}
            catch(...){errors_.fetch_add(1,std::memory_order_relaxed);}
        }
    }

    inline static std::atomic<std::uint64_t> next_id_{0};
    std::filesystem::path path_,temporary_;
    Writer writer_;
    std::atomic<std::uint64_t> errors_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string pending_;
    std::thread worker_;
    bool enabled_=false,stopping_=false,have_pending_=false;
};

} // namespace surfxr
