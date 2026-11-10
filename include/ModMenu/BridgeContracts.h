#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
namespace be::native {
enum class Scene { Unknown, Main, Pause };
inline constexpr char ModsEventName[] = "button.be_mods_open";
inline constexpr unsigned ModsEventId = [] {
    unsigned value = 2166136261u;
    for (char c : ModsEventName) if (c) value = (value ^ unsigned(c)) * 16777619u;
    return value;
}();
inline bool targetVersion(unsigned a, unsigned b, unsigned c, unsigned d) { return a==1 && b==26 && c==60 && d==5; }
inline std::uint64_t fingerprint(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}
inline bool modsPress(std::span<std::byte const> bytes) {
    if (bytes.size()<18) return false;
    unsigned kind{}, action{};
    std::memcpy(&kind, bytes.data(), 4);
    std::memcpy(&action, bytes.data()+8, 4);
    auto previous=std::to_integer<unsigned>(bytes[16]), now=std::to_integer<unsigned>(bytes[17]);
    return kind==1 && action==ModsEventId && previous<=2 && previous!=1 && now==1;
}
bool InsertModsDefinition(std::string const&, std::string&, std::string_view);
}
