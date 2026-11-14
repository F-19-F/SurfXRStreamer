#include "preference_format.hpp"
#include "surfxr/layout_store.hpp"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace surfxr {
const char* layout_store_status_name(LayoutStoreStatus status) noexcept {
    switch(status){case LayoutStoreStatus::Ok:return "ok";case LayoutStoreStatus::Missing:return "missing";
    case LayoutStoreStatus::Invalid:return "invalid";case LayoutStoreStatus::IoError:return "io_error";}
    return "io_error";
}
#ifndef _WIN32
namespace {
constexpr std::size_t max_file_bytes=16384;
struct File {int fd=-1;explicit File(int value):fd(value){}~File(){if(fd>=0)::close(fd);}File(const File&)=delete;File& operator=(const File&)=delete;};
struct Temporary {std::string path;~Temporary(){if(!path.empty())::unlink(path.c_str());}};
bool valid_path(const std::string& path){return !path.empty()&&path.size()<4096&&path.find('\0')==std::string::npos;}
bool read_pose(std::istream& in,UiPose& pose){auto& p=pose.position;auto& q=pose.orientation;return static_cast<bool>(in>>p.x>>p.y>>p.z>>q.x>>q.y>>q.z>>q.w);}
void write_pose(std::ostream& out,const UiPose& pose){const auto& p=pose.position;const auto& q=pose.orientation;out<<p.x<<' '<<p.y<<' '<<p.z<<' '<<q.x<<' '<<q.y<<' '<<q.z<<' '<<q.w;}
bool parse(const std::string& text,UiLayout& layout){
    if(!std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>=32&&c<=126)||c=='\n'||c=='\r'||c=='\t';}))return false;
    std::istringstream in(text);in.imbue(std::locale::classic());std::string word;int count=0,version=0;
    if(!(in>>word>>version)||!preference_format_matches(word,"LAYOUT")||(version<1||version>3))return false;
    layout.version=static_cast<std::uint32_t>(version);
    if(!(in>>word>>count)||word!="PANELS"||count<1||count>4)return false;
    layout.panels.resize(count);
    for(auto& panel:layout.panels){int visible=-1;std::int64_t source_id=-1;
        if(!(in>>word>>source_id>>visible>>panel.width_m)||word!="PANEL"||source_id<0||source_id>std::numeric_limits<std::uint32_t>::max()||(visible!=0&&visible!=1))return false;
        if(version>=2&&!(in>>panel.curvature_radians))return false;
        if(!read_pose(in,panel.anchor_pose))return false;
        panel.source_id=static_cast<std::uint32_t>(source_id);panel.visible=visible!=0;
    }
    int visible=-1;
    if(!(in>>word>>visible>>layout.keyboard_width_m)||word!="KEYBOARD"||(visible!=0&&visible!=1)||!read_pose(in,layout.keyboard_pose))return false;
    layout.keyboard_visible=visible!=0;
    if(version>=3&&(!(in>>word>>layout.keyboard_binding>>layout.keyboard_space_width_m)||word!="BINDING"||!read_pose(in,layout.keyboard_offset)))return false;
    if(!(in>>word)||word!="END")return false;
    in>>std::ws;
    return in.eof()&&validate_ui_layout(layout);
}
std::string encode(const UiLayout& layout){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"SURFXR_FRAME_LAYOUT 3\nPANELS "<<layout.panels.size()<<'\n';
    for(const auto& p:layout.panels){out<<"PANEL "<<p.source_id<<' '<<static_cast<int>(p.visible)<<' '<<p.width_m<<' '<<(layout.version==1?0.f:p.curvature_radians)<<' ';write_pose(out,p.anchor_pose);out<<'\n';}
    out<<"KEYBOARD "<<static_cast<int>(layout.keyboard_visible)<<' '<<layout.keyboard_width_m<<' ';write_pose(out,layout.keyboard_pose);out<<"\nBINDING "<<layout.keyboard_binding<<' '<<layout.keyboard_space_width_m<<' ';write_pose(out,layout.keyboard_offset);out<<"\nEND\n";
    return out.str();
}
}
#endif
LayoutStoreStatus load_layout(const std::string& path,UiLayout& output) noexcept {
#ifdef _WIN32
    (void)path;(void)output;return LayoutStoreStatus::IoError;
#else
    try{
        if(!valid_path(path))return LayoutStoreStatus::Invalid;
        File file(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(file.fd<0)return errno==ENOENT?LayoutStoreStatus::Missing:LayoutStoreStatus::IoError;
        struct stat st{};if(::fstat(file.fd,&st)<0)return LayoutStoreStatus::IoError;
        if(!S_ISREG(st.st_mode)||st.st_size<0||static_cast<std::uint64_t>(st.st_size)>max_file_bytes)return LayoutStoreStatus::Invalid;
        std::string data;char buffer[4096];
        for(;;){const auto count=::read(file.fd,buffer,sizeof(buffer));if(count<0){if(errno==EINTR)continue;return LayoutStoreStatus::IoError;}
            if(!count)break;
            if(data.size()+static_cast<std::size_t>(count)>max_file_bytes)return LayoutStoreStatus::Invalid;
            data.append(buffer,static_cast<std::size_t>(count));}
        UiLayout parsed;if(!parse(data,parsed))return LayoutStoreStatus::Invalid;
        output=std::move(parsed);return LayoutStoreStatus::Ok;
    }catch(...){return LayoutStoreStatus::IoError;}
#endif
}
LayoutStoreStatus save_layout(const std::string& path,const UiLayout& layout) noexcept {
#ifdef _WIN32
    (void)path;(void)layout;return LayoutStoreStatus::IoError;
#else
    try{
        if(!valid_path(path)||!validate_ui_layout(layout))return LayoutStoreStatus::Invalid;
        const std::filesystem::path target(path);
        if(target.filename().empty()||target.filename()=="."||target.filename()=="..")return LayoutStoreStatus::Invalid;
        auto parent=target.parent_path();if(parent.empty())parent=".";
        std::error_code error;const bool created=std::filesystem::create_directories(parent,error);
        if(error)return LayoutStoreStatus::IoError;
        if(created&&::chmod(parent.c_str(),0700)<0)return LayoutStoreStatus::IoError;
        auto temp_path=(parent/("."+target.filename().string()+".tmp.XXXXXX")).string();
        std::vector<char> name(temp_path.begin(),temp_path.end());name.push_back('\0');
        File file(::mkstemp(name.data()));if(file.fd<0)return LayoutStoreStatus::IoError;
        Temporary temp{std::string(name.data())};
        if(::fchmod(file.fd,0600)<0||::fcntl(file.fd,F_SETFD,FD_CLOEXEC)<0)return LayoutStoreStatus::IoError;
        const auto data=encode(layout);std::size_t sent=0;
        while(sent<data.size()){const auto count=::write(file.fd,data.data()+sent,data.size()-sent);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)return LayoutStoreStatus::IoError;
            sent+=static_cast<std::size_t>(count);}
        while(::fsync(file.fd)<0){if(errno!=EINTR)return LayoutStoreStatus::IoError;}
        const auto closed=::close(file.fd);file.fd=-1;if(closed<0)return LayoutStoreStatus::IoError;
        if(::rename(temp.path.c_str(),target.c_str())<0)return LayoutStoreStatus::IoError;
        temp.path.clear();return LayoutStoreStatus::Ok;
    }catch(...){return LayoutStoreStatus::IoError;}
#endif
}
} // namespace surfxr
