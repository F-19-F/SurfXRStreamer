#include "surfxr/localization.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#ifdef SURFXR_FRAME_TEXT
#include <json-c/json.h>
#endif

namespace surfxr::i18n {
namespace {
namespace fs=std::filesystem;
fs::path config_root() {
    if(const char* value=std::getenv("XDG_CONFIG_HOME");value&&fs::path(value).is_absolute())return value;
    if(const char* value=std::getenv("HOME"))return fs::path(value)/".config";
    return {};
}
std::string read_bounded(const fs::path& path,std::size_t limit,bool tail=false) {
    std::ifstream input(path,std::ios::binary);
    if(!input)return {};
    input.seekg(0,std::ios::end);const auto length=input.tellg();
    if(length<0||(!tail&&length>static_cast<std::streamoff>(limit)))return {};
    const auto count=std::min(static_cast<std::size_t>(length),limit);
    input.seekg(tail?length-static_cast<std::streamoff>(count):std::streampos(0));
    std::string text(count,'\0');input.read(text.data(),count);return input?text:std::string{};
}
std::string runtime_language() {
#ifdef SURFXR_FRAME_TEXT
    const auto root=config_root();if(root.empty())return "english";
    const auto text=read_bounded(root/"openvr/openvrpaths.vrpath",65536);
    auto* paths=json_tokener_parse(text.c_str());if(!paths)return "english";
    json_object* logs=nullptr;std::string language="english";
    if(json_object_object_get_ex(paths,"log",&logs)&&json_object_is_type(logs,json_type_array)) {
        for(std::size_t i=0;i<json_object_array_length(logs)&&i<8;++i) {
            auto* value=json_object_array_get_idx(logs,i);
            if(!json_object_is_type(value,json_type_string))continue;
            const fs::path directory=json_object_get_string(value);
            if(!directory.is_absolute())continue;
            // SteamVR publishes its resolved UI language in this diagnostic.
            // Read both ends: its startup line remains near the beginning of
            // long-running logs; a later announcement takes precedence.
            const auto path=directory/"vrserver.txt";
            auto found=steamvr_language_from_log(read_bounded(path,1024*1024,true));
            if(found.empty()) {
                std::ifstream input(path);std::string prefix(65536,'\0');
                input.read(prefix.data(),prefix.size());prefix.resize(static_cast<std::size_t>(input.gcount()));
                found=steamvr_language_from_log(prefix);
            }
            if(!found.empty()){language=found;break;}
        }
    }
    json_object_put(paths);return language;
#else
    return "english";
#endif
}
struct Cache {std::mutex mutex;std::chrono::steady_clock::time_point next{};std::string selection="auto";Language language=Language::English;};
Cache cache;
void refresh() {
    const auto now=std::chrono::steady_clock::now();if(now<cache.next)return;
    cache.next=now+std::chrono::seconds(1);
    auto selection=read_bounded(config_root()/"surfxr-client/ui-language",32);
    while(!selection.empty()&&(selection.back()=='\n'||selection.back()=='\r'))selection.pop_back();
    cache.selection=selection=="en"||selection=="zh-CN"?selection:"auto";
    cache.language=language_from_locale(cache.selection=="auto"?runtime_language():cache.selection);
}
}
std::string steamvr_language_from_log(std::string_view text) {
    constexpr std::string_view marker="Web language is ";
    const auto at=text.rfind(marker);if(at==text.npos)return {};
    const auto start=at+marker.size(),end=text.find_first_of("\r\n \t",start);
    const auto token=text.substr(start,end==text.npos?text.size()-start:end-start);
    if(token.empty()||token.size()>32)return {};
    for(char c:token)if(!((c>='a'&&c<='z')||c=='-'||c=='_'))return {};
    return std::string(token);
}
Language system_language(){std::lock_guard<std::mutex> lock(cache.mutex);refresh();return cache.language;}
std::string language_selection(){std::lock_guard<std::mutex> lock(cache.mutex);refresh();return cache.selection;}
void select_language(std::string_view selection) {
    if(selection!="auto"&&selection!="en"&&selection!="zh-CN")throw std::invalid_argument("invalid_ui_language");
    const auto root=config_root();if(root.empty())throw std::runtime_error("ui_config_directory_unavailable");
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto path=root/"surfxr-client/ui-language";
    fs::create_directories(path.parent_path());
    const auto temporary=path.string()+".tmp";
    {std::ofstream output(temporary,std::ios::trunc);output<<selection<<'\n';output.close();if(!output)throw std::runtime_error("ui_language_save_failed");}
    // This is a UI preference, separate from connection settings and IPC.
    std::error_code error;
#ifdef _WIN32
    if(!MoveFileExW(fs::path(temporary).c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        error=std::error_code(GetLastError(),std::system_category());
#else
    fs::rename(temporary,path,error);
#endif
    if(error){fs::remove(temporary);throw std::runtime_error("ui_language_save_failed");}
    cache.next={};refresh();
}
}
