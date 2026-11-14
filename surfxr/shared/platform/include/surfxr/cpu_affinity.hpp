#pragma once
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <initializer_list>
#include <sched.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace surfxr {
struct CpuAffinityStats {
    bool available_requested=false,attempted=false,applied=false,changed=false,effective_known=false;
    unsigned before_count=0,requested_count=0,effective_count=0;
    std::uint64_t before_mask_low64=0,requested_mask_low64=0,effective_mask_low64=0;
    int error=0;
};

// CPU IDs can contain holes after hotplug; an online CPU count is not a mask.
// Reject truncation/unsupported IDs instead of accidentally selecting fewer CPUs.
inline bool parse_online_cpu_list(std::string_view text,cpu_set_t& output) noexcept {
    if(text.size()>4096)return false;
    const auto whitespace=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
    while(!text.empty()&&whitespace(text.front()))text.remove_prefix(1);
    while(!text.empty()&&whitespace(text.back()))text.remove_suffix(1);
    if(text.empty())return false;
    cpu_set_t candidate;CPU_ZERO(&candidate);
    const char* cursor=text.data();const char* end=cursor+text.size();
    while(cursor!=end){
        unsigned first=0,last=0;
        const auto begin=std::from_chars(cursor,end,first);
        if(begin.ec!=std::errc{}||begin.ptr==cursor||first>=CPU_SETSIZE)return false;
        cursor=begin.ptr;last=first;
        if(cursor!=end&&*cursor=='-'){
            ++cursor;const auto finish=std::from_chars(cursor,end,last);
            if(finish.ec!=std::errc{}||finish.ptr==cursor||last<first||last>=CPU_SETSIZE)return false;
            cursor=finish.ptr;
        }
        for(unsigned cpu=first;cpu<=last;++cpu)CPU_SET(cpu,&candidate);
        if(cursor==end)break;
        if(*cursor++!=','||cursor==end)return false;
    }
    output=candidate;return true;
}

inline unsigned affinity_count(const cpu_set_t& mask) noexcept {
    unsigned count=0;for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&mask))++count;return count;
}
inline std::uint64_t affinity_low64(const cpu_set_t& mask) noexcept {
    std::uint64_t value=0;for(unsigned cpu=0;cpu<64&&cpu<CPU_SETSIZE;++cpu)
        if(CPU_ISSET(cpu,&mask))value|=std::uint64_t{1}<<cpu;
    return value;
}

inline bool parse_cpu_capacity(std::string_view text,unsigned& output) noexcept {
    if(text.size()>32)return false;
    const auto whitespace=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
    while(!text.empty()&&whitespace(text.front()))text.remove_prefix(1);
    while(!text.empty()&&whitespace(text.back()))text.remove_suffix(1);
    if(text.empty())return false;
    unsigned capacity=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),capacity);
    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||!capacity)return false;
    output=capacity;return true;
}

// Unknown capacities and homogeneous masks must keep the original policy.
// Select every equally strongest allowed CPU, never widen the allowed mask.
inline bool select_render_cpu_mask(const cpu_set_t& allowed,
                                  const std::array<unsigned,CPU_SETSIZE>& capacities,
                                  cpu_set_t& output) noexcept {
    unsigned minimum=0,maximum=0;
    for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&allowed)){
        const auto capacity=capacities[cpu];if(!capacity)return false;
        if(!minimum||capacity<minimum)minimum=capacity;
        if(capacity>maximum)maximum=capacity;
    }
    if(!minimum||minimum==maximum)return false;
    cpu_set_t selected;CPU_ZERO(&selected);
    for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)
        if(CPU_ISSET(cpu,&allowed)&&capacities[cpu]==maximum)CPU_SET(cpu,&selected);
    output=selected;return true;
}

// Called once after the viewer instance lock, before creating any threads or XR.
// Only the calling thread changes; subsequently created threads inherit it.
// Linux still intersects the request with cgroup cpuset restrictions. This does
// not change the daemon, dashboard, scheduler policy, priority, or cgroup limits.
inline CpuAffinityStats configure_cpu_affinity(bool available) noexcept {
    CpuAffinityStats result;result.available_requested=available;
    cpu_set_t before;CPU_ZERO(&before);
    if(sched_getaffinity(0,sizeof(before),&before)!=0){result.error=errno;return result;}
    result.before_count=result.requested_count=result.effective_count=affinity_count(before);
    result.before_mask_low64=result.requested_mask_low64=result.effective_mask_low64=affinity_low64(before);
    result.effective_known=true;
    if(!available)return result;
    const int fd=open("/sys/devices/system/cpu/online",O_RDONLY|O_CLOEXEC);
    if(fd<0){result.error=errno;return result;}
    char contents[4096];std::size_t size=0;
    while(size<sizeof(contents)){
        const auto count=read(fd,contents+size,sizeof(contents)-size);
        if(count<0&&errno==EINTR)continue;
        if(count<0){result.error=errno;break;}
        if(count==0)break;
        size+=static_cast<std::size_t>(count);
    }
    close(fd);
    if(result.error)return result;
    if(size==sizeof(contents)){result.error=EOVERFLOW;return result;}
    cpu_set_t requested;CPU_ZERO(&requested);
    if(!parse_online_cpu_list({contents,size},requested)){result.error=EINVAL;return result;}
    result.requested_count=affinity_count(requested);
    result.requested_mask_low64=affinity_low64(requested);
    if(CPU_EQUAL(&before,&requested))return result;
    result.attempted=true;
    if(sched_setaffinity(0,sizeof(requested),&requested)!=0){result.error=errno;return result;}
    result.applied=true;
    cpu_set_t effective;CPU_ZERO(&effective);
    if(sched_getaffinity(0,sizeof(effective),&effective)!=0){
        result.error=errno;result.effective_known=false;result.effective_count=0;result.effective_mask_low64=0;return result;
    }
    result.effective_count=affinity_count(effective);
    result.effective_mask_low64=affinity_low64(effective);
    result.changed=!CPU_EQUAL(&before,&effective);
    return result;
}

// Call on the XR thread only, after the network/decoder and persistence workers
// were created with the wider available mask. Linux affinity is per-thread;
// this does not repin existing workers or alter priorities, cpusets or governors.
// Missing/invalid CPU capacity data is an optional-policy failure, not fatal.
inline CpuAffinityStats configure_render_cpu_affinity(bool available) noexcept {
    CpuAffinityStats result;result.available_requested=available;
    cpu_set_t before;CPU_ZERO(&before);
    if(sched_getaffinity(0,sizeof(before),&before)!=0){result.error=errno;return result;}
    result.before_count=result.requested_count=result.effective_count=affinity_count(before);
    result.before_mask_low64=result.requested_mask_low64=result.effective_mask_low64=affinity_low64(before);
    result.effective_known=true;
    if(!available)return result;
    std::array<unsigned,CPU_SETSIZE> capacities{};
    for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&before)){
        char path[96];
        const auto length=std::snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%u/cpu_capacity",cpu);
        if(length<0||static_cast<std::size_t>(length)>=sizeof(path)){result.error=EOVERFLOW;return result;}
        const int fd=open(path,O_RDONLY|O_CLOEXEC);
        if(fd<0){result.error=errno;return result;}
        char contents[33];std::size_t size=0;
        while(size<sizeof(contents)){
            const auto count=read(fd,contents+size,sizeof(contents)-size);
            if(count<0&&errno==EINTR)continue;
            if(count<0){result.error=errno;break;}
            if(count==0)break;
            size+=static_cast<std::size_t>(count);
        }
        close(fd);
        if(result.error)return result;
        if(!parse_cpu_capacity({contents,size},capacities[cpu])){result.error=EINVAL;return result;}
    }
    cpu_set_t requested;CPU_ZERO(&requested);
    if(!select_render_cpu_mask(before,capacities,requested))return result;
    result.requested_count=affinity_count(requested);
    result.requested_mask_low64=affinity_low64(requested);
    result.attempted=true;
    if(sched_setaffinity(0,sizeof(requested),&requested)!=0){result.error=errno;return result;}
    result.applied=true;
    cpu_set_t effective;CPU_ZERO(&effective);
    if(sched_getaffinity(0,sizeof(effective),&effective)!=0){
        result.error=errno;result.effective_known=false;result.effective_count=0;result.effective_mask_low64=0;return result;
    }
    result.effective_count=affinity_count(effective);
    result.effective_mask_low64=affinity_low64(effective);
    result.changed=!CPU_EQUAL(&before,&effective);
    return result;
}

inline void cpu_affinity_parser_self_test(){
    cpu_set_t mask;CPU_ZERO(&mask);
    if(!parse_online_cpu_list("0-4,6-7\n",mask)||affinity_count(mask)!=7||CPU_ISSET(5,&mask)
       ||affinity_low64(mask)!=223)throw std::runtime_error("cpu_affinity_sparse_list_failed");
    if(!parse_online_cpu_list(" 2,2-3\n",mask)||affinity_count(mask)!=2||affinity_low64(mask)!=12)
        throw std::runtime_error("cpu_affinity_duplicate_list_failed");
    for(const auto text:{""," ","0-","4-2","0,","-1","0, 1","0-3tail","9999999999999999999999"}){
        if(parse_online_cpu_list(text,mask)||affinity_low64(mask)!=12)
            throw std::runtime_error("cpu_affinity_invalid_list_accepted");
    }
    char beyond[32];const auto limit=std::to_chars(beyond,beyond+sizeof(beyond),CPU_SETSIZE);
    if(parse_online_cpu_list({beyond,static_cast<std::size_t>(limit.ptr-beyond)},mask)||affinity_low64(mask)!=12)
        throw std::runtime_error("cpu_affinity_unrepresentable_cpu_accepted");
    // Read-only: inherit mode must preserve the caller's actual mask, including
    // a deliberate taskset restriction. This test never widens test processes.
    const auto inherited=configure_cpu_affinity(false);
    if(inherited.error||!inherited.effective_known||inherited.attempted||inherited.applied
       ||inherited.before_count==0||inherited.before_count!=inherited.effective_count
       ||inherited.before_mask_low64!=inherited.effective_mask_low64)
        throw std::runtime_error("cpu_affinity_inherit_changed_mask");
}
} // namespace surfxr
