#include "preference_format.hpp"
#include "surfxr/image_quality.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace surfxr {
namespace {
constexpr std::size_t max_bytes=1024;
constexpr const char* keys[]{"DESKTOP_FILTER","VR_UPSCALING","VR_SHARPENING","VR_SHARPENING_ENABLED","VR_TIMEWARP"};
bool valid_path(const std::string& path){return !path.empty()&&path.front()=='/'&&path.size()<4096&&path.find('\0')==std::string::npos;}
#ifndef _WIN32
struct File {int fd;explicit File(int value):fd(value){}~File(){if(fd>=0)::close(fd);}File(const File&)=delete;};
struct Temporary {std::string path;~Temporary(){if(!path.empty())::unlink(path.c_str());}};
bool sync_file(int fd){while(::fsync(fd)<0){if(errno!=EINTR)return false;}return true;}
#endif
ImageQualityStoreStatus save(const std::string& path,const ImageQualityPreferences& preferences) noexcept {
    if(!valid_path(path)||!valid_image_quality(preferences))return ImageQualityStoreStatus::Invalid;
#ifdef _WIN32
    return ImageQualityStoreStatus::IoError;
#else
    try{
        const std::filesystem::path target(path);const auto parent=target.parent_path();
        if(target.filename().empty()||target.filename()=="."||target.filename()=="..")return ImageQualityStoreStatus::Invalid;
        std::error_code error;const bool created=std::filesystem::create_directories(parent,error);
        if(error||(created&&::chmod(parent.c_str(),0700)<0))return ImageQualityStoreStatus::IoError;
        File directory(::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        if(directory.fd<0)return ImageQualityStoreStatus::IoError;
        const auto pattern=(parent/("."+target.filename().string()+".tmp.XXXXXX")).string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
        File file(::mkstemp(name.data()));if(file.fd<0)return ImageQualityStoreStatus::IoError;
        Temporary temporary{std::string(name.data())};
        if(::fchmod(file.fd,0600)<0||::fcntl(file.fd,F_SETFD,FD_CLOEXEC)<0)return ImageQualityStoreStatus::IoError;
        const auto text=encode_image_quality(preferences);std::size_t offset=0;
        while(offset<text.size()){
            const auto count=::write(file.fd,text.data()+offset,text.size()-offset);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)return ImageQualityStoreStatus::IoError;
            offset+=static_cast<std::size_t>(count);
        }
        if(!sync_file(file.fd))return ImageQualityStoreStatus::IoError;
        const auto closed=::close(file.fd);file.fd=-1;
        if(closed<0||::rename(temporary.path.c_str(),target.c_str())<0)return ImageQualityStoreStatus::IoError;
        temporary.path.clear();return sync_file(directory.fd)?ImageQualityStoreStatus::Ok:ImageQualityStoreStatus::IoError;
    }catch(...){return ImageQualityStoreStatus::IoError;}
#endif
}
}
bool operator==(const ImageQualityPreferences& a,const ImageQualityPreferences& b) noexcept {
    return a.desktop_filter==b.desktop_filter&&a.vr_upscaling==b.vr_upscaling&&a.vr_sharpening==b.vr_sharpening&&a.vr_sharpening_enabled==b.vr_sharpening_enabled&&a.vr_timewarp==b.vr_timewarp;
}
bool operator!=(const ImageQualityPreferences& a,const ImageQualityPreferences& b) noexcept{return !(a==b);}
bool valid_image_quality(const ImageQualityPreferences& p) noexcept{return p.desktop_filter>=0&&p.desktop_filter<=1&&p.vr_sharpening>=0&&p.vr_sharpening<=100;}
bool valid_image_quality_change(ImageQualityChange c) noexcept {
    switch(c.id){case ImageQualityId::DesktopFilter:case ImageQualityId::VrUpscaling:case ImageQualityId::VrSharpeningEnabled:case ImageQualityId::VrTimewarp:return c.value==0||c.value==1;
    case ImageQualityId::VrSharpening:return c.value>=0&&c.value<=100;}return false;
}
int image_quality_value(const ImageQualityPreferences& p,ImageQualityId id) noexcept {
    switch(id){case ImageQualityId::DesktopFilter:return p.desktop_filter;case ImageQualityId::VrUpscaling:return p.vr_upscaling;
    case ImageQualityId::VrSharpening:return p.vr_sharpening;case ImageQualityId::VrSharpeningEnabled:return p.vr_sharpening_enabled;case ImageQualityId::VrTimewarp:return p.vr_timewarp;}return -1;
}
bool apply_image_quality_change(ImageQualityPreferences& p,ImageQualityChange c) noexcept {
    if(!valid_image_quality(p)||!valid_image_quality_change(c))return false;
    switch(c.id){case ImageQualityId::DesktopFilter:p.desktop_filter=c.value;break;case ImageQualityId::VrUpscaling:p.vr_upscaling=c.value!=0;break;
    case ImageQualityId::VrSharpening:p.vr_sharpening=c.value;break;case ImageQualityId::VrSharpeningEnabled:p.vr_sharpening_enabled=c.value!=0;break;case ImageQualityId::VrTimewarp:p.vr_timewarp=c.value!=0;break;}return true;
}
const char* image_quality_store_status_name(ImageQualityStoreStatus s) noexcept {
    switch(s){case ImageQualityStoreStatus::Ok:return "ok";case ImageQualityStoreStatus::Missing:return "missing";case ImageQualityStoreStatus::Invalid:return "invalid";
    case ImageQualityStoreStatus::Disabled:return "disabled";case ImageQualityStoreStatus::IoError:return "io_error";}return "io_error";
}
std::string default_image_quality_path(){
    if(const auto* path=std::getenv("XDG_STATE_HOME");path&&valid_path(path))return (std::filesystem::path(path)/"surfxr-client/image-quality.conf").string();
    if(const auto* path=std::getenv("HOME");path&&valid_path(path))return (std::filesystem::path(path)/".local/state/surfxr-client/image-quality.conf").string();
    return {};
}
bool parse_image_quality(const std::string& text,ImageQualityPreferences& output) noexcept {
    try{
        if(text.size()>max_bytes||!std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>=32&&c<=126)||c=='\n'||c=='\r'||c=='\t';}))return false;
        std::istringstream input(text);input.imbue(std::locale::classic());std::string word;int version=0;
        if(!(input>>word>>version)||!preference_format_matches(word,"IMAGE_QUALITY")||(version!=1&&version!=2))return false;
        ImageQualityPreferences parsed;
        for(unsigned i=0;i<(version==1?4u:5u);++i){int value=0;if(!(input>>word>>value)||word!=keys[i]||!apply_image_quality_change(parsed,{static_cast<ImageQualityId>(i),value}))return false;}
        if(!(input>>word)||word!="END")return false;
        input>>std::ws;if(!input.eof())return false;output=parsed;return true;
    }catch(...){return false;}
}
std::string encode_image_quality(const ImageQualityPreferences& p){
    if(!valid_image_quality(p))throw std::invalid_argument("invalid_image_quality");
    std::ostringstream output;output.imbue(std::locale::classic());output<<"SURFXR_FRAME_IMAGE_QUALITY 2\n";
    for(unsigned i=0;i<5;++i)output<<keys[i]<<' '<<image_quality_value(p,static_cast<ImageQualityId>(i))<<'\n';
    output<<"END\n";return output.str();
}
ImageQualityStoreStatus load_image_quality(const std::string& path,ImageQualityPreferences& output) noexcept {
    if(path.empty())return ImageQualityStoreStatus::Disabled;
    if(!valid_path(path))return ImageQualityStoreStatus::Invalid;
#ifdef _WIN32
    (void)output;return ImageQualityStoreStatus::IoError;
#else
    try{
        File file(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(file.fd<0)return errno==ENOENT?ImageQualityStoreStatus::Missing:ImageQualityStoreStatus::IoError;
        struct stat info{};if(::fstat(file.fd,&info)<0)return ImageQualityStoreStatus::IoError;
        if(!S_ISREG(info.st_mode)||info.st_size<0||static_cast<std::uint64_t>(info.st_size)>max_bytes)return ImageQualityStoreStatus::Invalid;
        std::string text;char bytes[256];
        for(;;){const auto count=::read(file.fd,bytes,sizeof(bytes));if(count<0){if(errno==EINTR)continue;return ImageQualityStoreStatus::IoError;}
            if(count==0)break;
            if(text.size()+static_cast<std::size_t>(count)>max_bytes)return ImageQualityStoreStatus::Invalid;
            text.append(bytes,static_cast<std::size_t>(count));}
        return parse_image_quality(text,output)?ImageQualityStoreStatus::Ok:ImageQualityStoreStatus::Invalid;
    }catch(...){return ImageQualityStoreStatus::IoError;}
#endif
}
struct ImageQualityStore::Impl {
    explicit Impl(std::string value):path(std::move(value)){stats.enabled=!path.empty();if(stats.enabled)worker=std::thread([this]{run();});else stats.last_status=ImageQualityStoreStatus::Disabled;}
    std::string path;mutable std::mutex mutex;std::mutex finish_mutex;std::condition_variable condition;
    std::optional<ImageQualityPreferences> pending;ImageQualityStoreStats stats;bool stopping=false;std::thread worker;
    void run() noexcept {
        for(;;){ImageQualityPreferences next;{std::unique_lock<std::mutex> lock(mutex);condition.wait(lock,[this]{return stopping||pending.has_value();});if(!pending)break;next=*pending;pending.reset();}
            const auto result=save(path,next);std::lock_guard<std::mutex> lock(mutex);stats.last_status=result;if(result==ImageQualityStoreStatus::Ok)++stats.writes;else ++stats.failures;}
    }
};
ImageQualityStore::ImageQualityStore(std::string path):impl_(std::make_unique<Impl>(std::move(path))){}
ImageQualityStore::~ImageQualityStore(){finish();}
bool ImageQualityStore::submit(const ImageQualityPreferences& preferences) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!valid_image_quality(preferences)||!impl_->stats.enabled||impl_->stopping){++impl_->stats.rejected;return false;}
    if(impl_->pending)++impl_->stats.coalesced;
    impl_->pending=preferences;++impl_->stats.submitted;impl_->condition.notify_one();return true;
}
ImageQualityStoreStats ImageQualityStore::statistics()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->stats;}
ImageQualityStoreStats ImageQualityStore::finish() noexcept {
    std::lock_guard<std::mutex> finishing(impl_->finish_mutex);
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stopping=true;}impl_->condition.notify_one();if(impl_->worker.joinable())impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stats.finished=true;return impl_->stats;
}
} // namespace surfxr
