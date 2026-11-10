#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace be {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
extern HMODULE module;
fs::path directory();
std::string utf8(std::wstring_view);
std::wstring wide(std::string_view);
std::string readFile(fs::path const&);
bool writeFile(fs::path const&, std::string const&, std::string& error);
bool contained(fs::path const& base, fs::path const& child);
namespace log {
void write(char const* level, std::string const& text);
template<class... A> void info(std::format_string<A...> f, A&&... a) { write("INFO", std::format(f, std::forward<A>(a)...)); }
template<class... A> void warn(std::format_string<A...> f, A&&... a) { write("WARN", std::format(f, std::forward<A>(a)...)); }
template<class... A> void error(std::format_string<A...> f, A&&... a) { write("ERROR", std::format(f, std::forward<A>(a)...)); }
}
struct Mod {
    std::string id, name, version, author, description, license, parent, type, readme;
    std::string platform;
    std::vector<std::string> dependencies;
    std::map<std::string,std::string> links;
    fs::path folder, manifest, icon;
    Json metadata;
    std::vector<fs::path> configs;
    bool enabled{}, loaded{}, library{}, self{};
};
struct Preferences {
    bool libraries{}, disabled{true}, unloaded{true}, searchDescriptions{true};
    bool badges{true}, reverse{}, remember{true};
    bool updateFirst{};
    int fallbackKey{};
    double scale{1.0};
    std::string selected;
    std::string language{"auto"};
    void load(fs::path const&);
    void save(fs::path const&) const;
};
std::vector<Mod> scan(fs::path const& mods, fs::path const& self);
namespace assets {
void request(std::vector<Mod> const&);
void apply(std::vector<Mod>&);
}
std::vector<std::size_t> filter(std::vector<Mod> const&, std::string query, Preferences const&);
bool setEnabled(Mod const&, bool enabled, std::string& error);
enum class DependencyState { Enabled, Disabled, Missing };
DependencyState dependencyState(std::vector<Mod> const&, std::string const&);
struct PendingChange {Mod mod;bool original{},desired{};std::string manifestText;};
bool changeEnabled(Mod const&, bool, std::map<std::string,PendingChange>&,std::string&);
bool undoChanges(std::map<std::string,PendingChange>&,std::string&);
struct ConfigDocument {
    fs::path path;
    std::string original;
    Json draft;
    std::string error;
    bool dirty{};
};
std::vector<ConfigDocument> openConfigs(Mod const&);
bool saveConfigs(std::vector<ConfigDocument>&, std::string& message);
struct Update { std::string version, url, error; };
Update checkUpdate(Mod const&);
namespace ui {
float fontScale(float height);
void requestOpen(std::string mod = {}, bool config = false);
void close();
void toggle();
bool isOpen();
bool needsFrame();
bool blocksInput();
void draw(float width, float height);
void deviceLost();
void abort();
}
namespace native {
bool installMenuEntry();
bool hasNativeSession();
void menuApplied(bool open);
void setOverlayAvailable(bool available);
std::string menuStatus();
bool needsFallback();
}
namespace graphics { bool install(); void* image(fs::path const&); }
}
