#pragma once
#include <string>
#include <string_view>
namespace surfxr::i18n {
enum class Language { English, Chinese };
Language language_from_locale(std::string_view locale);
Language system_language();
std::string steamvr_language_from_log(std::string_view text);
std::string language_selection();
void select_language(std::string_view selection);
std::string translate(std::string_view text);
bool initialize_fonts();
}
