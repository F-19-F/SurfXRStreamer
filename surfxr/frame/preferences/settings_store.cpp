#include "preference_format.hpp"
#include "surfxr/settings_store.hpp"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace surfxr {
namespace {
constexpr std::size_t max_file_bytes=4096;
constexpr std::array<const char*,kRemoteSettingCount> keys{{
    "VR_FRAMERATE","VR_QUALITY","VR_BITRATE_MBPS","DESKTOP_FRAMERATE",
    "DESKTOP_BITRATE_MBPS","MICROPHONE_ENABLED","MICROPHONE_VOLUME","FOVEATED_STREAMING","RESERVED","DESKTOP_POINTER_PRIORITY"}};
bool valid_path(const std::string& path){
    return !path.empty()&&path.front()=='/'&&path.size()<4096&&path.find('\0')==std::string::npos;
}
#ifndef _WIN32
struct File {
    int fd=-1;
    explicit File(int value):fd(value){}
    ~File(){if(fd>=0)::close(fd);}
    File(const File&)=delete;
    File& operator=(const File&)=delete;
};
struct Temporary {std::string path;~Temporary(){if(!path.empty())::unlink(path.c_str());}};
bool sync_file(int fd){while(::fsync(fd)<0){if(errno!=EINTR)return false;}return true;}
#endif
SettingsStoreStatus save_settings(const std::string& path,const SettingsPreferences& preferences) noexcept {
    if(!valid_path(path)||!valid_settings_preferences(preferences))return SettingsStoreStatus::Invalid;
#ifdef _WIN32
    return SettingsStoreStatus::IoError;
#else
    try{
        const std::filesystem::path target(path);
        if(target.filename().empty()||target.filename()=="."||target.filename()=="..")return SettingsStoreStatus::Invalid;
        const auto parent=target.parent_path();
        std::error_code error;
        const bool created=std::filesystem::create_directories(parent,error);
        if(error)return SettingsStoreStatus::IoError;
        if(created&&::chmod(parent.c_str(),0700)<0)return SettingsStoreStatus::IoError;
        File directory(::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        if(directory.fd<0)return SettingsStoreStatus::IoError;
        const auto temporary_path=(parent/("."+target.filename().string()+".tmp.XXXXXX")).string();
        std::vector<char> name(temporary_path.begin(),temporary_path.end());name.push_back('\0');
        File file(::mkstemp(name.data()));
        if(file.fd<0)return SettingsStoreStatus::IoError;
        Temporary temporary{std::string(name.data())};
        if(::fchmod(file.fd,0600)<0||::fcntl(file.fd,F_SETFD,FD_CLOEXEC)<0)return SettingsStoreStatus::IoError;
        const auto data=encode_settings_preferences(preferences);
        std::size_t sent=0;
        while(sent<data.size()){
            const auto count=::write(file.fd,data.data()+sent,data.size()-sent);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)return SettingsStoreStatus::IoError;
            sent+=static_cast<std::size_t>(count);
        }
        if(!sync_file(file.fd))return SettingsStoreStatus::IoError;
        const auto closed=::close(file.fd);file.fd=-1;
        if(closed<0)return SettingsStoreStatus::IoError;
        if(::rename(temporary.path.c_str(),target.c_str())<0)return SettingsStoreStatus::IoError;
        temporary.path.clear();
        return sync_file(directory.fd)?SettingsStoreStatus::Ok:SettingsStoreStatus::IoError;
    }catch(...){return SettingsStoreStatus::IoError;}
#endif
}
} // namespace
const char* settings_store_status_name(SettingsStoreStatus status) noexcept {
    switch(status){
    case SettingsStoreStatus::Ok:return "ok";
    case SettingsStoreStatus::Missing:return "missing";
    case SettingsStoreStatus::Invalid:return "invalid";
    case SettingsStoreStatus::IoError:return "io_error";
    case SettingsStoreStatus::Disabled:return "disabled";
    }
    return "io_error";
}
std::string default_settings_path(){
    if(const auto* path=std::getenv("XDG_STATE_HOME");path&&valid_path(path))
        return (std::filesystem::path(path)/"surfxr-client/settings.conf").string();
    if(const auto* path=std::getenv("HOME");path&&valid_path(path))
        return (std::filesystem::path(path)/".local/state/surfxr-client/settings.conf").string();
    return {};
}
bool valid_settings_preferences(const SettingsPreferences& preferences) noexcept {
    for(std::size_t i=0;i<kRemoteSettingCount;++i){
        const auto v=preferences.values[i];
        if(preferences.surfxr_video&&i<3){
            if(!std::isfinite(v)||(i==0?(v<1||v>1000):i==1?(v<-2||v>7||v!=std::floor(v)):(v<0||v>1000)))return false;
        }else if(!valid_setting_change({static_cast<SettingId>(i),v}))return false;
    }
    return true;
}
bool requested_settings_preferences(const RemoteSettingsSnapshot& snapshot,SettingsPreferences& output) noexcept {
    SettingsPreferences preferences;
    for(std::size_t i=0;i<kRemoteSettingCount;++i)preferences.values[i]=snapshot.values[i].requested;
    preferences.surfxr_video=snapshot.capabilities.surfxr_video_controls;
    if(preferences.surfxr_video)for(std::size_t i=0;i<kRemoteSettingCount;++i)
        if((i<3||i==static_cast<std::size_t>(SettingId::DesktopPointerPriority))&&snapshot.values[i].pending&&snapshot.values[i].actual)
            preferences.values[i]=*snapshot.values[i].actual;
    if(!valid_settings_preferences(preferences))return false;
    output=preferences;
    return true;
}
bool parse_settings_preferences(const std::string& text,SettingsPreferences& output) noexcept {
    try{
        if(text.size()>max_file_bytes||!std::all_of(text.begin(),text.end(),[](unsigned char c){
            return (c>=32&&c<=126)||c=='\n'||c=='\r'||c=='\t';
        }))return false;
        std::istringstream input(text);input.imbue(std::locale::classic());
        std::string word;int version=0;
        if(!(input>>word>>version)||!preference_format_matches(word,"SETTINGS")||(version<1||version>5))return false;
        SettingsPreferences preferences;
        if(version>=4){int extended=0;if(!(input>>word>>extended)||word!="EVO_VIDEO"||(extended!=0&&extended!=1)||(version==4&&extended!=1))return false;preferences.surfxr_video=extended!=0;}
        for(std::size_t i=0;i<(version==1?7u:version==2?8u:version<5?9u:keys.size());++i)
            if(!(input>>word>>preferences.values[i])||word!=keys[i])return false;
        if(!(input>>word)||word!="END")return false;
        input>>std::ws;
        if(!input.eof()||!valid_settings_preferences(preferences))return false;
        output=preferences;
        return true;
    }catch(...){return false;}
}
std::string encode_settings_preferences(const SettingsPreferences& preferences){
    if(!valid_settings_preferences(preferences))throw std::invalid_argument("invalid_settings_preferences");
    std::ostringstream output;output.imbue(std::locale::classic());
    output<<std::setprecision(std::numeric_limits<double>::max_digits10)<<"SURFXR_FRAME_SETTINGS 5\nEVO_VIDEO "<<(preferences.surfxr_video?1:0)<<"\n";
    for(std::size_t i=0;i<keys.size();++i)output<<keys[i]<<' '<<preferences.values[i]<<'\n';
    output<<"END\n";
    return output.str();
}
SettingsStoreStatus load_settings(const std::string& path,SettingsPreferences& output) noexcept {
    if(path.empty())return SettingsStoreStatus::Disabled;
    if(!valid_path(path))return SettingsStoreStatus::Invalid;
#ifdef _WIN32
    (void)output;
    return SettingsStoreStatus::IoError;
#else
    try{
        File file(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(file.fd<0)return errno==ENOENT?SettingsStoreStatus::Missing:SettingsStoreStatus::IoError;
        struct stat info{};
        if(::fstat(file.fd,&info)<0)return SettingsStoreStatus::IoError;
        if(!S_ISREG(info.st_mode)||info.st_size<0||static_cast<std::uint64_t>(info.st_size)>max_file_bytes)
            return SettingsStoreStatus::Invalid;
        std::string data;char buffer[1024];
        for(;;){
            const auto count=::read(file.fd,buffer,sizeof(buffer));
            if(count<0){if(errno==EINTR)continue;return SettingsStoreStatus::IoError;}
            if(count==0)break;
            if(data.size()+static_cast<std::size_t>(count)>max_file_bytes)return SettingsStoreStatus::Invalid;
            data.append(buffer,static_cast<std::size_t>(count));
        }
        SettingsPreferences parsed;
        if(!parse_settings_preferences(data,parsed))return SettingsStoreStatus::Invalid;
        output=parsed;
        return SettingsStoreStatus::Ok;
    }catch(...){return SettingsStoreStatus::IoError;}
#endif
}
struct SettingsStore::Impl {
    explicit Impl(std::string value):path(std::move(value)){
        stats.enabled=!path.empty();
        if(stats.enabled)worker=std::thread([this]{run();});
        else stats.last_status=SettingsStoreStatus::Disabled;
    }
    std::string path;
    mutable std::mutex mutex;
    std::mutex finish_mutex;
    std::condition_variable condition;
    std::optional<SettingsPreferences> pending;
    SettingsStoreStats stats;
    bool stopping=false;
    std::thread worker;
    void run() noexcept {
        for(;;){
            SettingsPreferences next;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,[this]{return stopping||pending.has_value();});
                if(!pending)break;
                next=*pending;pending.reset();
            }
            const auto status=save_settings(path,next);
            {
                std::lock_guard<std::mutex> lock(mutex);
                stats.last_status=status;
                if(status==SettingsStoreStatus::Ok)++stats.writes;
                else ++stats.failures;
            }
        }
    }
};
SettingsStore::SettingsStore(std::string path):impl_(std::make_unique<Impl>(std::move(path))){}
SettingsStore::~SettingsStore(){finish();}
bool SettingsStore::submit(const RemoteSettingsSnapshot& snapshot) noexcept {
    SettingsPreferences preferences;
    const bool valid=requested_settings_preferences(snapshot,preferences);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!valid||!impl_->stats.enabled||impl_->stopping){++impl_->stats.rejected;return false;}
    if(impl_->pending)++impl_->stats.coalesced;
    impl_->pending=preferences;++impl_->stats.submitted;
    impl_->condition.notify_one();
    return true;
}
SettingsStoreStats SettingsStore::statistics() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}
SettingsStoreStats SettingsStore::finish() noexcept {
    // Separate serialization permits repeat/concurrent finish calls without
    // holding the queue lock across disk writes or thread join.
    std::lock_guard<std::mutex> finish_lock(impl_->finish_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping=true;
    }
    impl_->condition.notify_one();
    if(impl_->worker.joinable())impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stats.finished=true;
    return impl_->stats;
}
} // namespace surfxr
