#include "preference_format.hpp"
#include "surfxr/input_preferences.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
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
constexpr std::size_t max_bytes=4096;
constexpr const char* keys[]{"VR_CONTROLLERS_ENABLED","PERFORMANCE_OVERLAY","GAZE_DEBUG_OVERLAY","BOUNDARY_SYNC_ENABLED","SYNC_RAW_BATTERY",
    "MINI_HUD_ENABLED","MINI_HUD_FPS","MINI_HUD_LATENCY","MINI_HUD_FRAME_TIME","MINI_HUD_DECODE",
    "MINI_HUD_GAZE_ONLY","MINI_HUD_GAZE_MARKER","MINI_HUD_X","MINI_HUD_Y","MINI_HUD_SCALE","TRANSITION_HUD_ENABLED"};
constexpr bool InputPreferences::* flags[]{&InputPreferences::vr_controllers_enabled,&InputPreferences::performance_overlay,
    &InputPreferences::gaze_debug_overlay,&InputPreferences::boundary_sync_enabled,&InputPreferences::sync_raw_battery,
    &InputPreferences::mini_hud_enabled,&InputPreferences::mini_hud_fps,&InputPreferences::mini_hud_latency,
    &InputPreferences::mini_hud_frame_time,&InputPreferences::mini_hud_decode,&InputPreferences::mini_hud_gaze_only,&InputPreferences::mini_hud_gaze_marker};
constexpr int InputPreferences::* numbers[]{&InputPreferences::mini_hud_x,&InputPreferences::mini_hud_y,&InputPreferences::mini_hud_scale};
static_assert(std::size(keys)==kInputPreferenceCount&&std::size(flags)+std::size(numbers)+1==kInputPreferenceCount);
bool valid_path(const std::string& path){return !path.empty()&&path.front()=='/'&&path.size()<4096&&path.find('\0')==std::string::npos;}
#ifndef _WIN32
struct File {int fd;explicit File(int value):fd(value){}~File(){if(fd>=0)::close(fd);}File(const File&)=delete;};
struct Temporary {std::string path;~Temporary(){if(!path.empty())::unlink(path.c_str());}};
bool sync_file(int fd){while(::fsync(fd)<0){if(errno!=EINTR)return false;}return true;}
#endif
InputPreferencesStoreStatus save(const std::string& path,const InputPreferences& preferences) noexcept {
    if(!valid_path(path))return InputPreferencesStoreStatus::Invalid;
#ifdef _WIN32
    return InputPreferencesStoreStatus::IoError;
#else
    try{
        const std::filesystem::path target(path);const auto parent=target.parent_path();
        if(target.filename().empty()||target.filename()=="."||target.filename()=="..")return InputPreferencesStoreStatus::Invalid;
        std::error_code error;const bool created=std::filesystem::create_directories(parent,error);
        if(error||(created&&::chmod(parent.c_str(),0700)<0))return InputPreferencesStoreStatus::IoError;
        File directory(::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        if(directory.fd<0)return InputPreferencesStoreStatus::IoError;
        const auto pattern=(parent/("."+target.filename().string()+".tmp.XXXXXX")).string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
        File file(::mkstemp(name.data()));if(file.fd<0)return InputPreferencesStoreStatus::IoError;
        Temporary temporary{std::string(name.data())};
        if(::fchmod(file.fd,0600)<0||::fcntl(file.fd,F_SETFD,FD_CLOEXEC)<0)return InputPreferencesStoreStatus::IoError;
        const auto text=encode_input_preferences(preferences);std::size_t offset=0;
        while(offset<text.size()){
            const auto count=::write(file.fd,text.data()+offset,text.size()-offset);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)return InputPreferencesStoreStatus::IoError;
            offset+=static_cast<std::size_t>(count);
        }
        if(!sync_file(file.fd))return InputPreferencesStoreStatus::IoError;
        const auto closed=::close(file.fd);file.fd=-1;
        if(closed<0||::rename(temporary.path.c_str(),target.c_str())<0)return InputPreferencesStoreStatus::IoError;
        temporary.path.clear();return sync_file(directory.fd)?InputPreferencesStoreStatus::Ok:InputPreferencesStoreStatus::IoError;
    }catch(...){return InputPreferencesStoreStatus::IoError;}
#endif
}
}
bool valid_keyboard_shortcut(const KeyboardShortcut& chord) noexcept {
    bool ended=false;unsigned count=0;
    for(unsigned i=0;i<chord.size();++i){const auto code=chord[i];if(!code){ended=true;continue;}
        if(ended)return false;
        const bool valid=(code>=1&&code<=0x58&&code!=0x54&&code!=0x55)||
            code==0xe01c||code==0xe01d||code==0xe035||code==0xe038||code==0xe037||code==0xe045||
            code==0xe047||code==0xe048||code==0xe049||code==0xe04b||code==0xe04d||code==0xe04f||
            code==0xe050||code==0xe051||code==0xe052||code==0xe053||code==0xe05b||code==0xe05c||code==0xe05d;
        if(!valid||std::find(chord.begin(),chord.begin()+i,code)!=chord.begin()+i)return false;
        ++count;
    }
    return count>0;
}
bool operator==(const InputPreferences& a,const InputPreferences& b) noexcept {
    if(a.input_shortcuts!=b.input_shortcuts)return false;
    for(unsigned i=0;i<kInputPreferenceCount;++i){const auto id=static_cast<InputPreferenceId>(i);if(input_preference_value(a,id)!=input_preference_value(b,id))return false;}
    return true;
}
bool operator!=(const InputPreferences& a,const InputPreferences& b) noexcept{return !(a==b);}
bool valid_input_preference_change(InputPreferenceChange c) noexcept {
    const auto* spec=input_preference_spec(c.id);return spec&&c.value>=spec->minimum&&c.value<=spec->maximum;
}
int input_preference_value(const InputPreferences& p,InputPreferenceId id) noexcept {
    if(id==InputPreferenceId::TransitionHudEnabled)return p.transition_hud_enabled;
    const auto index=static_cast<unsigned>(id);if(index>=kInputPreferenceCount)return -1;
    return index<std::size(flags)?int(p.*flags[index]):p.*numbers[index-std::size(flags)];
}
bool apply_input_preference_change(InputPreferences& p,InputPreferenceChange c) noexcept {
    if(!valid_input_preference_change(c))return false;
    if(c.id==InputPreferenceId::TransitionHudEnabled){p.transition_hud_enabled=c.value!=0;return true;}
    const auto index=static_cast<unsigned>(c.id);
    if(index<std::size(flags))p.*flags[index]=c.value!=0;
    else p.*numbers[index-std::size(flags)]=c.value;
    return true;
}
const char* input_preferences_store_status_name(InputPreferencesStoreStatus s) noexcept {
    switch(s){case InputPreferencesStoreStatus::Ok:return "ok";case InputPreferencesStoreStatus::Missing:return "missing";case InputPreferencesStoreStatus::Invalid:return "invalid";
    case InputPreferencesStoreStatus::Disabled:return "disabled";case InputPreferencesStoreStatus::IoError:return "io_error";}return "io_error";
}
std::string default_input_preferences_path(){
    if(const auto* path=std::getenv("XDG_STATE_HOME");path&&valid_path(path))return (std::filesystem::path(path)/"surfxr-client/input-preferences.conf").string();
    if(const auto* path=std::getenv("HOME");path&&valid_path(path))return (std::filesystem::path(path)/".local/state/surfxr-client/input-preferences.conf").string();
    return {};
}
bool parse_input_preferences(const std::string& text,InputPreferences& output) noexcept {
    try{
        if(text.size()>max_bytes||!std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>=32&&c<=126)||c=='\n'||c=='\r'||c=='\t';}))return false;
        std::istringstream input(text);input.imbue(std::locale::classic());std::string word,version;
        if(!(input>>word>>version)||!preference_format_matches(word,"INPUT_PREFERENCES")||(version!="1"&&version!="2"&&version!="3"&&version!="4"&&version!="5"&&version!="6"&&version!="7"&&version!="8"&&version!="9"))return false;
        InputPreferences parsed;
        const auto count=(version=="7"||version=="8"||version=="9")?kInputPreferenceCount:version=="6"?15:static_cast<std::size_t>(version[0]-'0');
        for(unsigned i=0;i<count;++i){int value=0;if(!(input>>word>>value)||word!=keys[i]||!apply_input_preference_change(parsed,{static_cast<InputPreferenceId>(i),value}))return false;}
        if(version=="8")for(unsigned slot=0;slot<2;++slot){
            if(!(input>>word)||word!=(slot?"INPUT_MODE_SHORTCUT":"INPUT_METHOD_SHORTCUT"))return false;
            for(auto& code:parsed.input_shortcuts[slot]){unsigned value;if(!(input>>value)||value>65535)return false;code=static_cast<std::uint16_t>(value);}
            if(!valid_keyboard_shortcut(parsed.input_shortcuts[slot]))return false;
        }
        if(version=="8"){
            if(parsed.input_shortcuts[0]==KeyboardShortcut{0xe05b,0x39}&&parsed.input_shortcuts[1]==KeyboardShortcut{0x2a})parsed.input_shortcuts=kDefaultInputShortcuts;
            else{
                parsed.input_shortcuts.resize(2);
                for(const auto& chord:kDefaultInputShortcuts)if(std::find(parsed.input_shortcuts.begin(),parsed.input_shortcuts.end(),chord)==parsed.input_shortcuts.end())parsed.input_shortcuts.push_back(chord);
            }
        }
        if(version=="9"){
            unsigned size;if(!(input>>word>>size)||word!="SHORTCUTS"||size>kMaxKeyboardShortcuts)return false;
            parsed.input_shortcuts.assign(size,{});
            for(auto& chord:parsed.input_shortcuts){
                if(!(input>>word)||word!="SHORTCUT")return false;
                for(auto& code:chord){unsigned value;if(!(input>>value)||value>65535)return false;code=static_cast<std::uint16_t>(value);}
                if(!valid_keyboard_shortcut(chord))return false;
            }
        }
        if(!(input>>word)||word!="END")return false;
        input>>std::ws;if(!input.eof())return false;output=parsed;return true;
    }catch(...){return false;}
}
std::string encode_input_preferences(const InputPreferences& p){
    std::ostringstream output;output.imbue(std::locale::classic());output<<"SURFXR_FRAME_INPUT_PREFERENCES 9\n";
    for(unsigned i=0;i<kInputPreferenceCount;++i)output<<keys[i]<<' '<<input_preference_value(p,static_cast<InputPreferenceId>(i))<<'\n';
    if(p.input_shortcuts.size()>kMaxKeyboardShortcuts)throw std::invalid_argument("too_many_shortcuts");
    output<<"SHORTCUTS "<<p.input_shortcuts.size()<<'\n';
    for(const auto& chord:p.input_shortcuts){
        if(!valid_keyboard_shortcut(chord))throw std::invalid_argument("invalid_keyboard_shortcut");
        output<<"SHORTCUT";for(auto code:chord)output<<' '<<code;output<<'\n';
    }
    output<<"END\n";return output.str();
}
InputPreferencesStoreStatus load_input_preferences(const std::string& path,InputPreferences& output) noexcept {
    if(path.empty())return InputPreferencesStoreStatus::Disabled;
    if(!valid_path(path))return InputPreferencesStoreStatus::Invalid;
#ifdef _WIN32
    (void)output;return InputPreferencesStoreStatus::IoError;
#else
    try{
        File file(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(file.fd<0)return errno==ENOENT?InputPreferencesStoreStatus::Missing:InputPreferencesStoreStatus::IoError;
        struct stat info{};if(::fstat(file.fd,&info)<0)return InputPreferencesStoreStatus::IoError;
        if(!S_ISREG(info.st_mode)||info.st_size<0||static_cast<std::uint64_t>(info.st_size)>max_bytes)return InputPreferencesStoreStatus::Invalid;
        std::string text;char bytes[256];
        for(;;){const auto count=::read(file.fd,bytes,sizeof(bytes));if(count<0){if(errno==EINTR)continue;return InputPreferencesStoreStatus::IoError;}
            if(count==0)break;
            if(text.size()+static_cast<std::size_t>(count)>max_bytes)return InputPreferencesStoreStatus::Invalid;
            text.append(bytes,static_cast<std::size_t>(count));}
        return parse_input_preferences(text,output)?InputPreferencesStoreStatus::Ok:InputPreferencesStoreStatus::Invalid;
    }catch(...){return InputPreferencesStoreStatus::IoError;}
#endif
}
struct InputPreferencesStore::Impl {
    explicit Impl(std::string value):path(std::move(value)){stats.enabled=!path.empty();if(stats.enabled)worker=std::thread([this]{run();});else stats.last_status=InputPreferencesStoreStatus::Disabled;}
    std::string path;mutable std::mutex mutex;std::mutex finish_mutex;std::condition_variable condition;
    std::optional<InputPreferences> pending;InputPreferencesStoreStats stats;bool stopping=false;std::thread worker;
    void run() noexcept {
        for(;;){InputPreferences next;{std::unique_lock<std::mutex> lock(mutex);condition.wait(lock,[this]{return stopping||pending.has_value();});if(!pending)break;next=*pending;pending.reset();}
            const auto result=save(path,next);std::lock_guard<std::mutex> lock(mutex);stats.last_status=result;if(result==InputPreferencesStoreStatus::Ok)++stats.writes;else ++stats.failures;}
    }
};
InputPreferencesStore::InputPreferencesStore(std::string path):impl_(std::make_unique<Impl>(std::move(path))){}
InputPreferencesStore::~InputPreferencesStore(){finish();}
bool InputPreferencesStore::submit(const InputPreferences& preferences) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->stats.enabled||impl_->stopping){++impl_->stats.rejected;return false;}
    if(impl_->pending)++impl_->stats.coalesced;
    impl_->pending=preferences;++impl_->stats.submitted;impl_->condition.notify_one();return true;
}
InputPreferencesStoreStats InputPreferencesStore::statistics()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->stats;}
InputPreferencesStoreStats InputPreferencesStore::finish() noexcept {
    std::lock_guard<std::mutex> finishing(impl_->finish_mutex);
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stopping=true;}impl_->condition.notify_one();if(impl_->worker.joinable())impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stats.finished=true;return impl_->stats;
}
} // namespace surfxr
