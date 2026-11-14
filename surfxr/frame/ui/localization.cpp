#include "surfxr/localization.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
namespace surfxr::i18n {
namespace {
std::string normalized(std::string_view text) {
    std::string key; bool space=false;
    for(unsigned char c:text) {
        if(std::isspace(c)){space=!key.empty();continue;}
        if(space){key+=' ';space=false;}
        key+=static_cast<char>(std::tolower(c));
    }
    return key;
}
}
Language language_from_locale(std::string_view locale) {
    const auto end=locale.find_first_of("-_.@:");
    const auto tag=normalized(locale.substr(0,end));
    return tag=="zh"||tag=="schinese"||tag=="tchinese"?Language::Chinese:Language::English;
}
std::string translate(std::string_view text) {
    if(system_language()!=Language::Chinese)return std::string(text);
    static const std::unordered_map<std::string,std::string> catalog={
#include "translations_zh.hpp"
    };
    if(const auto found=catalog.find(normalized(text));found!=catalog.end())return found->second;
    // Translate separated presentation fragments, never IPC fields or addresses.
    for(const auto separator:{std::string_view(" | "),std::string_view(": ")}) {
        const auto at=text.find(separator);
        if(at!=std::string_view::npos)return translate(text.substr(0,at))+std::string(separator)+translate(text.substr(at+separator.size()));
    }
    return std::string(text);
}
}
