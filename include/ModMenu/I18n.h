#pragma once
#include <string>
#include <string_view>
namespace be::i18n {
std::string normalize(std::string_view);
void select(std::string_view);
void observeGameLanguage(std::string_view);
std::string locale();
char const* tr(char const*);
std::string glyphText();
}
