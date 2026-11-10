#include "ModMenu/I18n.h"
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace be::i18n {
namespace {
enum Language { Auto, Japanese, English, Chinese };
std::atomic<int> selection{Auto}, game{Auto};
struct Translation {char const* ja;char const* en;char const* zh;};
constexpr Translation translations[]={
#include "Translations.inc"
};
Language language(std::string_view code) {
    if(code=="ja" || code=="ja_JP" || code=="ja-JP")return Japanese;
    if(code=="zh" || code=="zh_CN" || code=="zh-CN" || code=="zh_Hans" || code=="zh-Hans")return Chinese;
    return English;
}
int effective() {
    if(auto value=selection.load();value!=Auto)return value;
    if(auto value=game.load();value!=Auto)return value;
    auto id=PRIMARYLANGID(GetUserDefaultUILanguage());
    return id==LANG_JAPANESE?Japanese:id==LANG_CHINESE && SUBLANGID(GetUserDefaultUILanguage())==SUBLANG_CHINESE_SIMPLIFIED?Chinese:English;
}
}
std::string normalize(std::string_view code) {
    if(code=="auto" || code.empty())return "auto";
    if(code=="ja_JP" || code=="ja" || code=="ja-JP")return "ja_JP";
    if(code=="en_US" || code=="en" || code=="en-US")return "en_US";
    if(code=="zh_CN" || code=="zh" || code=="zh-CN" || code=="zh_Hans" || code=="zh-Hans")return "zh_CN";
    return "auto";
}
void select(std::string_view code) {auto normalized=normalize(code);selection=normalized=="auto"?Auto:language(normalized);}
void observeGameLanguage(std::string_view code) {game=language(code);}
std::string locale() {switch(effective()){case Japanese:return "ja_JP";case Chinese:return "zh_CN";default:return "en_US";}}
char const* tr(char const* text) {
    int current=effective();if(current==Japanese)return text;
    for(auto const& entry:translations)if(std::strcmp(text,entry.ja)==0)return current==Chinese?entry.zh:entry.en;
    return text;
}
std::string glyphText() {
    std::string result="日本語简体中文";
    for(auto const& entry:translations){result+=entry.ja;result+=entry.en;result+=entry.zh;}
    return result;
}
}
