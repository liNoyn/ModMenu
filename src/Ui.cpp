// ModMenu screen.
//
// Visual language ("Classic"): a full-screen dimmed backdrop instead of a
// framed window, dark list wells with thin separators, centered title, stone
// buttons with a light top edge and a dark bottom edge (white outline and
// warm text on hover), drop-shadowed text, cycling "Label: value" buttons
// instead of switches, and dark tooltips with a violet edge. Status is shown
// by small flat labels under each name.
#include "ModMenu/App.h"
#include "ModMenu/I18n.h"
#include <shellapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <thread>

namespace be::ui {
namespace {
#ifdef BE_PREVIEW
std::map<std::string, ImVec2> previewRows;
#endif

// ------------------------------------------------------------------ metrics
float unit = 1.f;
float s(float value) { return value * unit; }
// One "pixel" of the classic look: crisp edges at every scale.
float px() { return std::max(1.f, std::floor(unit * 1.5f)); }

// ------------------------------------------------------------------ look
namespace mc {
constexpr ImU32 rgb(unsigned hex, unsigned a = 255) { return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, a); }
inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

constexpr ImU32 White   = rgb(0xFFFFFF);
constexpr ImU32 Gray    = rgb(0xA8A8A8);
constexpr ImU32 Dim     = rgb(0x808080);
constexpr ImU32 Black   = rgb(0x000000);
constexpr ImU32 Warm    = rgb(0xFFFFA0); // hovered button text
constexpr ImU32 On      = rgb(0x55FF55);
constexpr ImU32 Off     = rgb(0xFF5555);
constexpr ImU32 Gold    = rgb(0xFFAA00);
constexpr ImU32 Link    = rgb(0x5C8DFF);
constexpr ImU32 Well    = rgb(0x000000, 125);
constexpr ImU32 Stone   = rgb(0x6C6C6C);
constexpr ImU32 StoneHi = rgb(0xA6A6A6);
constexpr ImU32 StoneLo = rgb(0x464646);

ImFont* font(int role) {
    auto& fonts = ImGui::GetIO().Fonts->Fonts;
    return role < fonts.Size ? fonts[role] : ImGui::GetFont();
}
ImFont* body() { return font(0); }
ImFont* large() { return font(1); }
ImFont* strong() { return font(2); }

ImU32 shade(ImU32 c, float k) {
    auto v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32({v.x * k, v.y * k, v.z * k, v.w});
}

// Text with the classic offset shadow (a quarter of the text's brightness).
void say(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, char const* text, float wrap = 0) {
    float o = px();
    dl->AddText(f, size, {p.x + o, p.y + o}, shade(c, .25f), text, nullptr, wrap);
    dl->AddText(f, size, p, c, text, nullptr, wrap);
}
void say(ImDrawList* dl, ImVec2 p, ImU32 c, std::string const& text, ImFont* f = nullptr) {
    f = f ? f : body();
    say(dl, f, f->FontSize, p, c, text.c_str());
}
ImVec2 measure(std::string const& text, ImFont* f = nullptr, float wrap = 0) {
    f = f ? f : body();
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap, text.c_str());
}
// Text placed by the layout (so it scrolls), with shadow.
void text(std::string const& value, ImU32 c = White, ImFont* f = nullptr, float wrap = -1) {
    f        = f ? f : body();
    float w  = wrap < 0 ? ImGui::GetContentRegionAvail().x : wrap;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 z = f->CalcTextSizeA(f->FontSize, FLT_MAX, w, value.c_str());
    say(ImGui::GetWindowDrawList(), f, f->FontSize, p, c, value.c_str(), w);
    ImGui::Dummy({std::max(z.x, 1.f), z.y});
}

std::string fit(ImFont* f, std::string value, float maxWidth) {
    if (auto nl = value.find('\n'); nl != std::string::npos) value.resize(nl);
    if (f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, value.c_str()).x <= maxWidth) return value;
    float dots = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, "...").x;
    while (!value.empty() && f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, value.c_str()).x + dots > maxWidth) {
        auto last = value.size() - 1;
        while (last && (static_cast<unsigned char>(value[last]) & 0xC0) == 0x80) --last;
        value.resize(last);
    }
    return value + "...";
}

void stone(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool enabled) {
    float o = px();
    dl->AddRectFilled(a, b, hovered ? White : Black);
    ImVec2 ia{a.x + o, a.y + o}, ib{b.x - o, b.y - o};
    if (!enabled) {
        dl->AddRectFilled(ia, ib, rgb(0x2C2C2C));
        return;
    }
    dl->AddRectFilled(ia, ib, Stone);
    dl->AddRectFilled(ia, {ib.x, ia.y + o}, StoneHi);
    dl->AddRectFilled(ia, {ia.x + o, ib.y}, rgb(0x8E8E8E));
    dl->AddRectFilled({ia.x, ib.y - o * 1.5f}, ib, StoneLo);
}

struct Hit { bool pressed, hovered, held; };
Hit hit(char const* id, ImVec2 size, bool enabled) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton("##hit", size);
    ImGui::EndDisabled();
    Hit h{pressed && enabled, enabled && ImGui::IsItemHovered(), enabled && ImGui::IsItemActive()};
    ImGui::PopID();
    if (h.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return h;
}

float buttonHeight() { return std::round(s(36)); }
float buttonWidth(std::string const& label) { return measure(label).x + s(36); }

bool button(char const* id, std::string const& label, ImVec2 size = {}, bool enabled = true, ImU32 ink = White) {
    if (size.x <= 0) size.x = buttonWidth(label);
    if (size.y <= 0) size.y = buttonHeight();
    ImVec2 a = ImGui::GetCursorScreenPos(), b{a.x + size.x, a.y + size.y};
    Hit    h = hit(id, size, enabled);
    auto*  dl = ImGui::GetWindowDrawList();
    stone(dl, a, b, h.hovered, enabled);
    auto   t  = fit(body(), label, size.x - s(12));
    ImVec2 ts = measure(t);
    ImVec2 tp{std::round(a.x + (size.x - ts.x) * .5f), std::round(a.y + (size.y - ts.y) * .5f - px() * .5f)};
    if (!enabled) dl->AddText(tp, rgb(0xA0A0A0), t.c_str());
    else say(dl, body(), body()->FontSize, tp, h.hovered && ink == White ? Warm : ink, t.c_str());
    return h.pressed;
}

// "Label: value" button; the value keeps its own color.
bool cycle(char const* id, std::string const& label, std::string const& value, ImU32 valueInk, ImVec2 size = {}) {
    std::string prefix = label.empty() ? "" : label + ": ";
    if (size.x <= 0) size.x = buttonWidth(prefix + value);
    if (size.y <= 0) size.y = buttonHeight();
    ImVec2 a = ImGui::GetCursorScreenPos(), b{a.x + size.x, a.y + size.y};
    Hit    h = hit(id, size, true);
    auto*  dl = ImGui::GetWindowDrawList();
    stone(dl, a, b, h.hovered, true);
    float  w = measure(prefix + value).x;
    ImVec2 tp{std::round(a.x + (size.x - w) * .5f), std::round(a.y + (size.y - body()->FontSize) * .5f - px() * .5f)};
    say(dl, body(), body()->FontSize, tp, h.hovered ? Warm : White, prefix.c_str());
    say(dl, body(), body()->FontSize, {tp.x + measure(prefix).x, tp.y}, valueInk, value.c_str());
    return h.pressed;
}

// Square button with a drawn glyph.
bool glyphButton(char const* id, float side, std::function<void(ImDrawList*, ImVec2, float, ImU32)> const& glyph, bool active = false) {
    ImVec2 a = ImGui::GetCursorScreenPos(), b{a.x + side, a.y + side};
    Hit    h = hit(id, {side, side}, true);
    auto*  dl = ImGui::GetWindowDrawList();
    stone(dl, a, b, h.hovered || active, true);
    glyph(dl, {(a.x + b.x) * .5f, (a.y + b.y) * .5f - px() * .5f}, side, h.hovered ? Warm : White);
    return h.pressed;
}
void funnel(ImDrawList* dl, ImVec2 c, float side, ImU32 ink) {
    float r = side * .26f;
    dl->AddTriangleFilled({c.x - r, c.y - r * .8f}, {c.x + r, c.y - r * .8f}, {c.x, c.y + r * .1f}, ink);
    dl->AddRectFilled({c.x - r * .18f, c.y}, {c.x + r * .18f, c.y + r * .9f}, ink);
}
void sliders(ImDrawList* dl, ImVec2 c, float side, ImU32 ink) {
    float r = side * .28f, t = px();
    for (int i = -1; i <= 1; ++i) {
        float y = c.y + i * r * .7f;
        dl->AddRectFilled({c.x - r, y - t * .5f}, {c.x + r, y + t * .5f}, ink);
        float k = c.x + (i == 0 ? r * .45f : -r * .35f);
        dl->AddRectFilled({k - t * 1.5f, y - t * 2}, {k + t * 1.5f, y + t * 2}, ink);
    }
}
void cross(ImDrawList* dl, ImVec2 c, float side, ImU32 ink) {
    float r = side * .2f;
    dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, ink, px() * 1.5f);
    dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, ink, px() * 1.5f);
}

bool field(char const* id, char* buffer, std::size_t size, char const* hint, float width) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {s(8), std::round((buttonHeight() - body()->FontSize) * .5f)});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(Black));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, v4(Black));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, v4(Black));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(Gray));
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputTextWithHint(id, hint, buffer, size);
    if (ImGui::IsItemActive()) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), White, 0, 0, px());
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return changed;
}

// Dark list area with a light rule above and a dark rule below.
void well(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    float o = px();
    dl->AddRectFilled(a, b, Well);
    dl->AddRectFilled({a.x, a.y - o}, {b.x, a.y}, rgb(0x000000, 200));
    dl->AddRectFilled({a.x, a.y}, {b.x, a.y + o}, rgb(0xFFFFFF, 40));
    dl->AddRectFilled({a.x, b.y - o}, {b.x, b.y}, rgb(0xFFFFFF, 40));
}

void tooltip(std::string const& value) {
    if (value.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(s(380));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0;
    st.WindowBorderSize = 0;
    st.ChildBorderSize  = 0;
    st.PopupBorderSize  = px() * 1.5f;
    st.FrameBorderSize  = px();
    st.WindowPadding    = {s(10), s(8)};
    st.FramePadding     = {s(8), s(6)};
    st.ItemSpacing      = {s(6), s(6)};
    st.ItemInnerSpacing = {s(6), s(6)};
    st.CellPadding      = {s(4), s(2)};
    st.ScrollbarSize    = std::round(s(7));
    st.GrabMinSize      = s(12);
    ImVec4* c                        = st.Colors;
    c[ImGuiCol_Text]                 = v4(White);
    c[ImGuiCol_TextDisabled]         = v4(Dim);
    c[ImGuiCol_WindowBg]             = v4(rgb(0x101010, 240));
    c[ImGuiCol_ChildBg]              = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg]              = v4(rgb(0x100010, 245));
    c[ImGuiCol_Border]               = v4(rgb(0x5000FF, 200));
    c[ImGuiCol_FrameBg]              = v4(Black);
    c[ImGuiCol_FrameBgHovered]       = v4(Black);
    c[ImGuiCol_FrameBgActive]        = v4(Black);
    c[ImGuiCol_ScrollbarBg]          = v4(rgb(0x000000, 160));
    c[ImGuiCol_ScrollbarGrab]        = v4(rgb(0x808080));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(rgb(0xC0C0C0));
    c[ImGuiCol_ScrollbarGrabActive]  = v4(rgb(0xC0C0C0));
    c[ImGuiCol_SliderGrab]           = v4(rgb(0xC6C6C6));
    c[ImGuiCol_SliderGrabActive]     = v4(White);
    c[ImGuiCol_CheckMark]            = v4(White);
    c[ImGuiCol_Header]               = v4(rgb(0xFFFFFF, 40));
    c[ImGuiCol_HeaderHovered]        = v4(rgb(0xFFFFFF, 60));
    c[ImGuiCol_HeaderActive]         = v4(rgb(0xFFFFFF, 80));
    c[ImGuiCol_TextSelectedBg]       = v4(rgb(0x3050FF, 160));
    c[ImGuiCol_NavHighlight]         = v4(White);
    c[ImGuiCol_ModalWindowDimBg]     = v4(rgb(0x000000, 170));
}
} // namespace mc

// ------------------------------------------------------------------ state
std::atomic<int>  command{};
std::atomic<bool> opened{};
Preferences       preferences;
bool              initialized{};
std::vector<Mod>  catalog;
std::string       selected, notice;
std::array<char, 256> query{};
enum class Page { Mods, Config, Preferences } page{};
std::vector<ConfigDocument>   documents;
int                           documentIndex{};
std::mutex                    updateMutex;
std::map<std::string, Update> updates;
std::set<std::string>         updating;
bool                          askToggle{}, showFilters{};
bool focusSearch{};
std::string keyboardSelection;
std::atomic<bool> bulkChecking{};
std::map<std::string,PendingChange> pendingChanges;
float                         pendingScale{-1};

fs::path settingsPath() { return directory() / L"config" / L"menu.json"; }
void     persist() {
    preferences.selected = selected;
    preferences.save(settingsPath());
}
void refresh() {
    catalog = scan(directory().parent_path(), directory());
#ifdef BE_PREVIEW
    if(GetEnvironmentVariableW(L"MODMENU_PREVIEW_SNAPSHOT_DATA",nullptr,0))for(auto& mod:catalog) {
        mod.loaded=mod.enabled;
        mod.readme=readFile(mod.folder/L"README.md");
    }
#endif
    assets::request(catalog);
    if (std::none_of(catalog.begin(), catalog.end(), [](auto const& mod) { return mod.id == selected; })) selected.clear();
    if (selected.empty()) {
        auto visible = filter(catalog, "", preferences);
        if (!visible.empty()) selected = catalog[visible.front()].id;
    }
}
Mod* selection() {
    for (auto& mod : catalog)
        if (mod.id == selected) return &mod;
    return nullptr;
}
void openUrl(std::string const& value) {
    if (value.starts_with("https://") || value.starts_with("http://")) ShellExecuteW(nullptr, L"open", wide(value).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void openFolder(fs::path const& folder) { ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
std::string descriptionFor(Mod const& mod) {
    if(mod.description.empty())return i18n::tr("説明はありません。");
    return mod.self?i18n::tr(mod.description.c_str()):mod.description;
}
std::string linkLabel(std::string const& key) {
    if (key == "source" || key == "sources" || key == "repository") return i18n::tr("ソースコード");
    if (key == "issues") return i18n::tr("不具合の報告");
    if (key == "website" || key == "homepage") return i18n::tr("ウェブサイト");
    if (key == "discord") return "Discord";
    if (key == "wiki" || key == "docs") return i18n::tr("ドキュメント");
    return key;
}
bool updateAvailable(Mod const& mod) {
    std::lock_guard lock(updateMutex);
    auto it = updates.find(mod.id);
    return it != updates.end() && !it->second.version.empty() && it->second.version != mod.version && it->second.version != "v" + mod.version;
}

std::vector<std::size_t> visibleMods() {
    auto indices=filter(catalog,query.data(),preferences);
    if(preferences.updateFirst) {
        std::set<std::string> available;
        {std::lock_guard lock(updateMutex);for(auto const& mod:catalog){auto it=updates.find(mod.id);if(it!=updates.end() && !it->second.version.empty() && it->second.version!=mod.version && it->second.version!="v"+mod.version)available.insert(mod.id);}}
        std::stable_partition(indices.begin(),indices.end(),[&](auto i){return available.contains(catalog[i].id);});
    }
    return indices;
}
void checkAllUpdates() {
    if(bulkChecking.exchange(true))return;
    auto copies=catalog;
    std::thread([copies=std::move(copies)] {
        for(auto const& mod:copies) {
            {std::lock_guard lock(updateMutex);if(!updating.insert(mod.id).second)continue;}
            Update result;try{result=checkUpdate(mod);}catch(...){result.error=i18n::tr("更新情報の取得に失敗しました。");}
            {std::lock_guard lock(updateMutex);updates[mod.id]=std::move(result);updating.erase(mod.id);}
        }
        bulkChecking=false;
    }).detach();
}

// ------------------------------------------------------------------ icon
ImU32 tileColor(std::string const& name) {
    static ImU32 const palette[] = {mc::rgb(0x2D6CDF), mc::rgb(0xC0392B), mc::rgb(0x27AE60), mc::rgb(0x8E44AD), mc::rgb(0xD35400), mc::rgb(0x16A085)};
    unsigned h = 2166136261u;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    return palette[h % std::size(palette)];
}
void drawIcon(ImDrawList* dl, Mod const& mod, ImVec2 a, float side) {
    ImVec2 b{a.x + side, a.y + side};
    if (auto texture = graphics::image(mod.icon)) dl->AddImage((ImTextureID)(std::intptr_t)texture, a, b);
    else {
        dl->AddRectFilled(a, b, tileColor(mod.name));
        dl->AddRectFilled(a, {b.x, a.y + px()}, mc::rgb(0xFFFFFF, 70));
        dl->AddRectFilled({a.x, b.y - px() * 2}, b, mc::rgb(0x000000, 90));
        if (!mod.name.empty()) {
            unsigned    c   = 0;
            char const* beg = mod.name.c_str();
            int         n   = ImTextCharFromUtf8(&c, beg, beg + mod.name.size());
            std::string letter(beg, std::size_t(n > 0 ? n : 1));
            if (letter.size() == 1) letter[0] = char(std::toupper(static_cast<unsigned char>(letter[0])));
            ImFont* f  = mc::large();
            float   fs = side * .55f;
            ImVec2  ts = f->CalcTextSizeA(fs, FLT_MAX, 0, letter.c_str());
            mc::say(dl, f, fs, {a.x + (side - ts.x) * .5f, a.y + (side - ts.y) * .5f}, mc::White, letter.c_str());
        }
    }
    if (!mod.enabled) dl->AddRectFilled(a, b, mc::rgb(0x000000, 110));
}

// ------------------------------------------------------------------ tags
// Small flat labels under the name. Only what matters: an update, a disabled
// mod, and which loader/side it belongs to. "Not loaded" lives in the details.
struct Tag { std::string label; ImU32 fill, edge, ink; };
bool dependsOn(Mod const& mod, char const* name) {
    return std::any_of(mod.dependencies.begin(), mod.dependencies.end(), [&](auto const& d) { return _stricmp(d.c_str(), name) == 0; });
}
std::vector<Tag> tagsFor(Mod const& mod) {
    std::vector<Tag> out;
    if (updateAvailable(mod)) out.push_back({i18n::tr("更新:"), mc::rgb(0x3D7A2F), mc::rgb(0x274F1E), mc::White});
    if (!mod.enabled) out.push_back({i18n::tr("無効"), mc::rgb(0x8A3333), mc::rgb(0x5A2020), mc::White});
    if (mod.type == "native" || dependsOn(mod, "LeviLamina")) out.push_back({"LeviLamina", mc::rgb(0x34599E), mc::rgb(0x223B69), mc::White});
    else if (mod.type == "preload-native") out.push_back({"PreLoader", mc::rgb(0x3E5F66), mc::rgb(0x293F44), mc::White});
    if (mod.library) out.push_back({i18n::tr("ライブラリ"), mc::rgb(0x4A4A4A), mc::rgb(0x2E2E2E), mc::rgb(0xD8D8D8)});
    if (mod.platform == "client") out.push_back({"Client", mc::rgb(0x4A4A4A), mc::rgb(0x2E2E2E), mc::rgb(0xD8D8D8)});
    else if (mod.platform == "server") out.push_back({"Server", mc::rgb(0x4A4A4A), mc::rgb(0x2E2E2E), mc::rgb(0xD8D8D8)});
    return out;
}
float tagSize() { return std::round(mc::body()->FontSize * .78f); }
float tagHeight() { return tagSize() + std::round(s(4)); }
float tagWidth(Tag const& t) { return mc::body()->CalcTextSizeA(tagSize(), FLT_MAX, 0, t.label.c_str()).x + std::round(s(8)); }
// Draws labels left to right from p; stops before maxX. Returns the x after the last one.
float drawTags(ImDrawList* dl, std::vector<Tag> const& tags, ImVec2 p, float maxX) {
    float x = std::round(p.x), y = std::round(p.y), size = tagSize(), h = tagHeight();
    for (auto const& t : tags) {
        float w = std::round(tagWidth(t));
        if (x + w > maxX) break;
        dl->AddRectFilled({x, y}, {x + w, y + h}, t.fill);
        dl->AddRect({x, y}, {x + w, y + h}, t.edge, 0, 0, px());
        dl->AddText(mc::body(), size, {x + std::round(s(4)), std::round(y + (h - size) * .5f)}, t.ink, t.label.c_str());
        x += w + std::round(s(3));
    }
    return x;
}

// ------------------------------------------------------------------ option rows (settings pages)
struct Column { float x0{}, width{}; };
Column centered(float maxWidth) {
    float avail = ImGui::GetContentRegionAvail().x;
    float w     = std::min(avail, maxWidth);
    return {ImGui::GetCursorPosX() + (avail - w) * .5f, w};
}
Column nested(Column c, float by) { return {c.x0 + by, std::max(c.width - by, s(240))}; }

void groupTitle(Column const& c, std::string const& title, std::string const& description = {}) {
    ImGui::Dummy({0, s(8)});
    ImGui::SetCursorPosX(c.x0);
    mc::text(title, mc::Gold, mc::strong(), c.width);
    if (!description.empty()) {
        ImGui::SetCursorPosX(c.x0);
        mc::text(description, mc::Gray, nullptr, c.width);
    }
    ImGui::Dummy({0, s(2)});
}

// One option: label (and description) on the left, control on the right. Hovered rows light up.
template <class F>
void optionRow(Column const& c, char const* id, std::string const& label, std::string const& description, std::string const& tip, F&& control) {
    ImGui::PushID(id);
    auto*  dl     = ImGui::GetWindowDrawList();
    float  ctrlW  = std::min(s(300), c.width * .42f);
    float  y0     = ImGui::GetCursorPosY();
    ImVec2 origin{ImGui::GetWindowPos().x + c.x0 - ImGui::GetScrollX(), ImGui::GetCursorScreenPos().y};
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    float textW = c.width - ctrlW - s(24);
    ImGui::SetCursorPos({c.x0 + s(8), y0 + std::round((mc::buttonHeight() - mc::body()->FontSize) * .5f) + s(4)});
    ImGui::BeginGroup();
    mc::text(label, mc::White, nullptr, textW);
    mc::tooltip(tip);
    if (!description.empty()) mc::text(description, mc::Dim, nullptr, textW);
    ImGui::EndGroup();
    float textBottom = ImGui::GetItemRectMax().y;
    ImGui::SetCursorPos({c.x0 + c.width - ctrlW - s(6), y0 + s(4)});
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(ctrlW);
    control(ctrlW);
    ImGui::EndGroup();
    float bottom = std::max(textBottom, ImGui::GetItemRectMax().y) + s(4);
    dl->ChannelsSetCurrent(0);
    if (ImGui::IsMouseHoveringRect(origin, {origin.x + c.width, bottom}, false)) dl->AddRectFilled(origin, {origin.x + c.width, bottom}, mc::rgb(0xFFFFFF, 18));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos({origin.x, bottom + s(2)});
    ImGui::Dummy({c.width, 0});
    ImGui::PopID();
}

bool onOff(char const* id, bool& value, float width, bool enabled = true) {
    if (!enabled) {
        mc::button(id, value ? i18n::tr("オン") : i18n::tr("オフ"), {width, 0}, false);
        return false;
    }
    if (mc::cycle(id, "", value ? i18n::tr("オン") : i18n::tr("オフ"), value ? mc::On : mc::Off, {width, 0})) {
        value = !value;
        return true;
    }
    return false;
}

// Classic-looking inputs: black field, gray edge.
void pushInputStyle() {
    ImGui::PushStyleColor(ImGuiCol_Border, mc::v4(mc::Gray));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, mc::v4(mc::Black));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {s(8), std::round((mc::buttonHeight() - mc::body()->FontSize) * .5f) - px()});
}
void popInputStyle() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}
// Slider drawn as a dark track with a stone handle.
bool slider(char const* id, double& v, double lo, double hi, char const* format) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, mc::v4(mc::rgb(0x202020)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, mc::v4(mc::rgb(0x2A2A2A)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, mc::v4(mc::rgb(0x2A2A2A)));
    ImGui::PushStyleColor(ImGuiCol_Border, mc::v4(mc::Black));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {s(8), std::round((mc::buttonHeight() - mc::body()->FontSize) * .5f) - px()});
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, s(14));
    bool changed = ImGui::SliderScalar(id, ImGuiDataType_Double, &v, &lo, &hi, format);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return changed;
}

// ------------------------------------------------------------------ JSON config editor
std::string translated(Json const& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object()) {
        auto locale=i18n::locale();
        for (auto const& key : {locale,locale.substr(0,2),std::string("en_US"),std::string("en"),std::string("ja_JP"),std::string("zh_CN")})
            if (value.contains(key) && value[key].is_string()) return value[key].get<std::string>();
        for (auto const& field : value)
            if (field.is_string()) return field.get<std::string>();
    }
    return {};
}
Json optionSpec(Mod const& mod, std::string const& key) {
    try {
        auto files = mod.metadata.at("config").at("files");
        auto path  = documents.empty() ? "" : utf8(documents[std::size_t(documentIndex)].path.lexically_relative(mod.folder).generic_wstring());
        for (auto const& file : files) {
            if (file.value("path", std::string{}) != path || !file.contains("sections")) continue;
            for (auto const& section : file["sections"])
                if (section.contains("options"))
                    for (auto const& option : section["options"]) {
                        if (!option.is_object()) continue;
                        auto candidate = option.value("key", std::string{});
                        if (candidate == key) return option;
                        if (!candidate.empty() && candidate.front() == '/') {
                            std::string dotted = candidate.substr(1);
                            std::replace(dotted.begin(), dotted.end(), '/', '.');
                            if (dotted == key) return option;
                        }
                    }
        }
    } catch (...) {
    }
    return Json::object();
}
// "showFps" / "show_fps" -> "Show fps"; the raw key stays in the tooltip.
std::string humanize(std::string const& key) {
    if (key.empty() || key[0] == '#') return key;
    std::string out;
    for (std::size_t i = 0; i < key.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(key[i]);
        if (c == '_' || c == '-') {
            out += ' ';
            continue;
        }
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(key[i - 1]))) {
            out += ' ';
            out += char(std::tolower(c));
            continue;
        }
        out += char(c);
    }
    if (!out.empty() && static_cast<unsigned char>(out[0]) < 0x80) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

bool editValue(Column const& c, Json& value, Mod const& mod, std::string const& key, std::string label, int depth) {
    if (depth > 12) {
        ImGui::SetCursorPosX(c.x0);
        mc::text(i18n::tr("階層が深いため、ファイルから直接編集してください。"), mc::Gray);
        return false;
    }
    auto spec = optionSpec(mod, key);
    label     = humanize(label);
    if (spec.contains("label"))
        if (auto name = translated(spec["label"]); !name.empty()) label = name;
    std::string description = spec.contains("description") ? translated(spec["description"]) : std::string{};
    bool        readOnly    = spec.value("readOnly", false);
    bool        changed     = false;
    ImGui::PushID(key.c_str());

    if (value.is_object() || value.is_array()) {
        Column inner = c;
        if (depth > 0) {
            groupTitle(c, value.is_array() ? label + "  [" + std::to_string(value.size()) + "]" : label, description);
            inner = nested(c, s(18));
        }
        if (value.empty()) {
            ImGui::SetCursorPosX(inner.x0 + s(8));
            mc::text(i18n::tr("(空)"), mc::Dim);
        }
        for (auto& [name, child] : value.items())
            changed = editValue(inner, child, mod, key.empty() ? name : key + "." + name, value.is_array() ? "#" + name : name, depth + 1) || changed;
        ImGui::PopID();
        return changed;
    }

    optionRow(c, "row", label, description, key, [&](float width) {
        if (spec.contains("choices") && spec["choices"].is_array() && !spec["choices"].empty()) {
            // Choices cycle on click, like a classic options button.
            auto const& choices = spec["choices"];
            std::size_t current = 0;
            std::string shown   = value.dump();
            for (std::size_t i = 0; i < choices.size(); ++i)
                if (choices[i].is_object() && choices[i].contains("value") && choices[i]["value"] == value) {
                    current = i;
                    shown   = choices[i].contains("label") ? translated(choices[i]["label"]) : shown;
                }
            if (readOnly) mc::button("##ro", shown, {width, 0}, false);
            else if (mc::cycle("##choice", "", shown, mc::White, {width, 0})) {
                auto const& next = choices[(current + 1) % choices.size()];
                if (next.is_object() && next.contains("value")) {
                    value   = next["value"];
                    changed = true;
                }
            }
            return;
        }
        if (value.is_boolean()) {
            bool flag = value.get<bool>();
            if (onOff("##flag", flag, width, !readOnly)) {
                value   = flag;
                changed = true;
            }
            return;
        }
        ImGui::BeginDisabled(readOnly);
        if (value.is_number() && spec.contains("min") && spec.contains("max") && spec["min"].is_number() && spec["max"].is_number()) {
            double v = value.get<double>();
            ImGui::SetNextItemWidth(width);
            if (slider("##number", v, spec["min"].get<double>(), spec["max"].get<double>(), value.is_number_float() ? "%.3f" : "%.0f")) {
                if (value.is_number_float()) value = v;
                else value = (std::int64_t)std::llround(v);
                changed = true;
            }
        } else {
            pushInputStyle();
            ImGui::SetNextItemWidth(width);
            if (value.is_number_unsigned()) {
                auto n = value.get<std::uint64_t>();
                if (ImGui::InputScalar("##number", ImGuiDataType_U64, &n)) { value = n; changed = true; }
            } else if (value.is_number_integer()) {
                auto n = value.get<std::int64_t>();
                if (ImGui::InputScalar("##number", ImGuiDataType_S64, &n)) { value = n; changed = true; }
            } else if (value.is_number_float()) {
                auto n = value.get<double>();
                if (ImGui::InputDouble("##number", &n, 0, 0, "%.6g")) { value = n; changed = true; }
            } else if (value.is_string()) {
                auto str = value.get<std::string>();
                if (str.size() > 60000) mc::text(i18n::tr("長いテキストのため、ファイルから編集してください。"), mc::Gray);
                else {
                    std::vector<char> buffer(std::max<std::size_t>(4096, str.size() + 256), 0);
                    std::memcpy(buffer.data(), str.data(), str.size());
                    if (ImGui::InputText("##text", buffer.data(), buffer.size())) {
                        value   = std::string(buffer.data());
                        changed = true;
                    }
                }
            } else mc::text("null", mc::Dim);
            popInputStyle();
        }
        ImGui::EndDisabled();
    });
    ImGui::PopID();
    return changed;
}

void configPage(ImVec2 a, ImVec2 b) {
    auto* mod = selection();
    auto* dl  = ImGui::GetWindowDrawList();
    float top = a.y;
    if (mod && documents.size() > 1) {
        // File tabs across the top; the current one is shown pressed (disabled).
        ImGui::SetCursorScreenPos(a);
        float x = a.x;
        for (std::size_t i = 0; i < documents.size(); ++i) {
            auto name = utf8(documents[i].path.lexically_relative(mod->folder).filename().wstring());
            if (documents[i].dirty) name += " *";
            float w = std::min(mc::buttonWidth(name), s(240));
            if (x + w > b.x) break;
            ImGui::SetCursorScreenPos({x, a.y});
            ImGui::PushID(int(i));
            if (mc::button("##tab", name, {w, 0}, documentIndex != int(i))) documentIndex = int(i);
            ImGui::PopID();
            x += w + s(4);
        }
        top += mc::buttonHeight() + s(10);
    }
    mc::well(dl, {a.x - s(16), top}, {b.x + s(16), b.y});
    ImGui::SetCursorScreenPos({a.x, top + s(6)});
    ImGui::BeginChild("options", {b.x - a.x, b.y - top - s(10)});
    Column column = centered(s(900));
    if (!mod) {
        ImGui::SetCursorPosX(column.x0);
        mc::text(i18n::tr("MODが見つかりません。"), mc::Gray);
    } else if (documents.empty()) {
        ImGui::SetCursorPosX(column.x0);
        mc::text(i18n::tr("このMODには編集できる設定ファイルがありません。"), mc::Gray);
    } else {
        auto& doc = documents[std::size_t(documentIndex)];
        ImGui::SetCursorPosX(column.x0);
        mc::text(utf8(doc.path.lexically_relative(mod->folder).wstring()), mc::Dim);
        if (!doc.error.empty()) {
            ImGui::SetCursorPosX(column.x0);
            mc::text(doc.error, mc::Off);
        } else doc.dirty = editValue(column, doc.draft, *mod, "", i18n::tr("設定"), 0) || doc.dirty;
    }
    ImGui::Dummy({0, s(10)});
    ImGui::EndChild();
}

void preferencesPage(ImVec2 a, ImVec2 b) {
    mc::well(ImGui::GetWindowDrawList(), {a.x - s(16), a.y}, {b.x + s(16), b.y});
    ImGui::SetCursorScreenPos({a.x, a.y + s(6)});
    ImGui::BeginChild("preferences", {b.x - a.x, b.y - a.y - s(10)});
    Column c       = centered(s(860));
    bool   changed = false;
    optionRow(c,"language",i18n::tr("言語"),i18n::tr("表示言語を変更します。ゲーム追従が利用できない場合はWindowsの言語を使います。"),{},[&](float w){
        auto const& code=preferences.language;
        char const* name=code=="ja_JP"?"日本語":code=="en_US"?"English":code=="zh_CN"?"简体中文":i18n::tr("ゲームに合わせる");
        if(mc::button("language-choice",name,{w,0})) {
            preferences.language=code=="auto"?"ja_JP":code=="ja_JP"?"en_US":code=="en_US"?"zh_CN":"auto";
            i18n::select(preferences.language);changed=true;
        }
    });
    auto   flag    = [&](char const* id, char const* label, char const* description, bool& value) {
        optionRow(c, id, label, description, {}, [&](float w) { changed |= onOff("##v", value, w); });
    };
    groupTitle(c,i18n::tr("メニューの接続"));
    ImGui::SetCursorPosX(c.x0);mc::text(native::menuStatus(),mc::Gray,nullptr,c.width);
    optionRow(c,"fallback",i18n::tr("ボタン未追加時の代替キー"),i18n::tr("初期状態はオフ。ボタンの追加を確認できない場合だけ有効です。"),{},[&](float w){
        std::string label=preferences.fallbackKey?"F"+std::to_string(preferences.fallbackKey-VK_F1+1):i18n::tr("オフ");
        if(mc::button("fallback-key",label,{w,0})) {
            preferences.fallbackKey=preferences.fallbackKey==0?VK_F6:preferences.fallbackKey==VK_F10?0:preferences.fallbackKey+1;changed=true;
        }
    });
    groupTitle(c, i18n::tr("一覧"));
    flag("libs", i18n::tr("ライブラリを表示"), i18n::tr("他のMODが使う前提MODも一覧に出します。"), preferences.libraries);
    flag("disabled", i18n::tr("無効なMODを表示"), i18n::tr("無効にしたMODも一覧に残します。"), preferences.disabled);
    flag("unloaded", i18n::tr("未ロードのMODを表示"), i18n::tr("読み込まれていないMODも一覧に出します。"), preferences.unloaded);
    flag("badges", i18n::tr("ラベルを表示"), i18n::tr("名前の下に Update・Disabled・LeviLamina などを表示します。"), preferences.badges);
    groupTitle(c, i18n::tr("検索と操作"));
    flag("search", i18n::tr("説明文も検索"), i18n::tr("MOD名に加えて説明文からも探します。"), preferences.searchDescriptions);
    flag("remember", i18n::tr("選んだMODを記憶"), i18n::tr("次に開いたとき前回のMODを選んだ状態にします。"), preferences.remember);
    groupTitle(c, i18n::tr("画面"));
    if (pendingScale < 0) pendingScale = float(preferences.scale);
    optionRow(c, "scale", i18n::tr("表示サイズ"), i18n::tr("ドラッグを離したときに反映します。"), {}, [&](float w) {
        double v = pendingScale;
        ImGui::SetNextItemWidth(w);
        if (slider("##scale", v, .75, 1.75, "%.2f")) pendingScale = float(v);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            preferences.scale = pendingScale;
            changed           = true;
        }
    });
    optionRow(c, "keys", i18n::tr("操作"), i18n::tr("Mods ボタンで開きます。上下キーで選択、Enterで設定、Ctrl+Fで検索、Escで戻ります。ダブルクリックでも設定を開けます。"), {}, [](float) {});
    if (changed) persist();
    ImGui::EndChild();
}

// ------------------------------------------------------------------ mod list (left)
void openConfiguration(Mod const& mod) {
    if (mod.configs.empty()) {
        notice = i18n::tr("このMODには編集できる設定ファイルがありません。");
        return;
    }
    documents = openConfigs(mod);
    documentIndex = 0;
    page = Page::Config;
    notice.clear();
}
void modRow(Mod const& mod, float width) {
    auto*  dl     = ImGui::GetWindowDrawList();
    float  iconS  = std::round(s(40));
    float  lineH  = mc::body()->FontSize;
    float  height = std::max(iconS + s(10), s(4) + lineH + s(4) + tagHeight() + s(3) + lineH + s(6));
    float  indent = mod.parent.empty() ? 0 : s(18);
    ImVec2 p      = ImGui::GetCursorScreenPos();
    ImGui::PushID(mod.id.c_str());
    if (ImGui::InvisibleButton("##row", {width, height})) {
        selected = mod.id;
        persist();
    }
    bool hovered = ImGui::IsItemHovered();
    if(keyboardSelection==mod.id){ImGui::SetScrollHereY(.5f);keyboardSelection.clear();}
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        selected = mod.id;
        persist();
        openConfiguration(mod);
    }
#ifdef BE_PREVIEW
    previewRows[mod.id] = {p.x + width * .5f, p.y + height * .5f};
#endif
    ImGui::PopID();
    bool   active = selected == mod.id;
    ImVec2 a{p.x + indent, p.y}, b{p.x + width, p.y + height};
    if (active) {
        dl->AddRectFilled(a, b, mc::Black);
        dl->AddRect(a, b, mc::rgb(0x808080), 0, 0, px());
    } else if (hovered) dl->AddRectFilled(a, b, mc::rgb(0xFFFFFF, 22));
    if (indent > 0) dl->AddRectFilled({p.x + s(7), p.y}, {p.x + s(7) + px(), p.y + height * .5f}, mc::rgb(0x808080));

    ImVec2 ip{a.x + s(5), a.y + s(5)};
    drawIcon(dl, mod, ip, iconS);
    float tx = ip.x + iconS + s(8), right = b.x - s(6);
    dl->PushClipRect(a, {right, b.y}, true);
    // Line 1: name and version.
    float y    = a.y + s(4);
    auto  name = mc::fit(mc::body(), mod.name, (right - tx) * .7f);
    mc::say(dl, {tx, y}, mod.enabled ? mc::White : mc::Gray, name);
    if (!mod.version.empty()) {
        float vx = tx + mc::measure(name).x + s(8);
        mc::say(dl, {vx, y}, mc::Dim, mc::fit(mc::body(), mod.version, std::max(0.f, right - vx)));
    }
    y += lineH + s(4);
    // Line 2: chips. Line 3: description (two lines when there are no chips).
    auto tags = preferences.badges ? tagsFor(mod) : std::vector<Tag>{};
    if (!tags.empty()) {
        drawTags(dl, tags, {tx, y}, right);
        y += tagHeight() + s(3);
    }
    auto desc = descriptionFor(mod);
    std::replace(desc.begin(), desc.end(), '\n', ' ');
    if (!tags.empty()) mc::say(dl, {tx, y}, mc::Dim, mc::fit(mc::body(), desc, right - tx));
    else {
        dl->PushClipRect({tx, y}, {right, std::min(b.y - s(2), y + lineH * 2 + s(2))}, true);
        mc::say(dl, mc::body(), lineH, {tx, y}, mc::Dim, desc.c_str(), right - tx);
        dl->PopClipRect();
    }
    dl->PopClipRect();
}

void modList(ImVec2 a, ImVec2 b) {
    auto* dl    = ImGui::GetWindowDrawList();
    float width = b.x - a.x, side = mc::buttonHeight();
    ImGui::SetCursorScreenPos(a);
    if(focusSearch){ImGui::SetKeyboardFocusHere();focusSearch=false;}
    mc::field("##query", query.data(), query.size(), i18n::tr("MODを検索..."), width - side - s(6));
    ImGui::SameLine(0, s(6));
    if (mc::glyphButton("filters", side, mc::funnel, showFilters)) showFilters = !showFilters;
    mc::tooltip(i18n::tr("並び替えと絞り込み"));
    float y = a.y + side + s(8);
    if (showFilters) {
        // Cycling buttons, two per row.
        float half = (width - s(6)) * .5f;
        auto  at   = [&](int i) { ImGui::SetCursorScreenPos({a.x + (i % 2) * (half + s(6)), y + (i / 2) * (side + s(6))}); };
        bool  changed = false;
        at(0);
        if (mc::cycle("sort", i18n::tr("並び順"), preferences.updateFirst ? i18n::tr("更新あり優先") : preferences.reverse ? i18n::tr("名前 Z-A") : i18n::tr("名前 A-Z"), mc::White, {half, side})) {
            if(preferences.updateFirst){preferences.updateFirst=false;preferences.reverse=false;}
            else if(preferences.reverse){preferences.updateFirst=true;preferences.reverse=false;}
            else preferences.reverse=true;
            changed=true;
        }
        at(1);
        if (mc::cycle("libs", i18n::tr("ライブラリ"), preferences.libraries ? i18n::tr("表示") : i18n::tr("非表示"), preferences.libraries ? mc::On : mc::Off, {half, side})) preferences.libraries = !preferences.libraries, changed = true;
        at(2);
        if (mc::cycle("dis", i18n::tr("無効なMOD"), preferences.disabled ? i18n::tr("表示") : i18n::tr("非表示"), preferences.disabled ? mc::On : mc::Off, {half, side})) preferences.disabled = !preferences.disabled, changed = true;
        at(3);
        if (mc::cycle("unl", i18n::tr("未ロード"), preferences.unloaded ? i18n::tr("表示") : i18n::tr("非表示"), preferences.unloaded ? mc::On : mc::Off, {half, side})) preferences.unloaded = !preferences.unloaded, changed = true;
        if (changed) persist();
        y += (side + s(6)) * 2 + s(2);
    }
    auto visible = visibleMods();
    mc::say(dl, {a.x, y}, mc::White, std::to_string(visible.size()) + i18n::tr(" 個のMODを表示中") + (visible.size() != catalog.size() ? i18n::tr("（全 ") + std::to_string(catalog.size()) + i18n::tr(" 個）") : ""));
    y += mc::body()->FontSize + s(10);

    float wellPad = s(6);
    mc::well(dl, {a.x - wellPad, y}, {b.x + wellPad, b.y});
    ImGui::SetCursorScreenPos({a.x - wellPad + s(2), y + s(4)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, s(2)});
    ImGui::BeginChild("catalog", {width + wellPad * 2 - s(4), b.y - y - s(8)});
    float rowW = ImGui::GetContentRegionAvail().x - s(2);
    if (visible.empty()) {
        ImGui::Dummy({0, s(10)});
        ImGui::SetCursorPosX(s(10));
        mc::text(query[0] ? i18n::tr("検索に一致するMODはありません。") : i18n::tr("表示できるMODはありません。"), mc::Gray);
    }
    for (auto index : visible) modRow(catalog[index], rowW);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ details (right)
void label(std::string const& title) {
    ImGui::Dummy({0, s(8)});
    mc::text(title, mc::Gray);
}

void readme(std::string const& source) {
    std::istringstream lines(source);
    std::string        line;
    bool               code = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("```")) {
            code = !code;
            continue;
        }
        if (code) {
            ImGui::Indent(s(10));
            mc::text(line.empty() ? " " : line, mc::rgb(0xB8D8B8));
            ImGui::Unindent(s(10));
            continue;
        }
        if (auto first = line.find_first_not_of(" \t"); first != std::string::npos) {
            std::string_view t(line.c_str() + first);
            if (t.starts_with("![") || t.starts_with("<") || t.starts_with("[!") || t.starts_with("---") || t.starts_with("[![")) continue;
        }
        if (line.starts_with(">")) {
            while (line.starts_with(">")) line.erase(0, line.size() > 1 && line[1] == ' ' ? 2 : 1);
            if (line.empty() || line.starts_with("[!")) continue;
            ImGui::Indent(s(10));
            mc::text(line, mc::Gray);
            ImGui::Unindent(s(10));
            continue;
        }
        if (line.starts_with("#")) {
            auto level = line.find_first_not_of('#');
            auto begin = line.find_first_not_of("# ");
            if (begin == std::string::npos) continue;
            ImGui::Dummy({0, s(4)});
            mc::text(line.substr(begin), level <= 1 ? mc::White : mc::Gold, level <= 1 ? mc::large() : mc::strong());
            continue;
        }
        if (line.empty()) {
            ImGui::Dummy({0, s(4)});
            continue;
        }
        std::size_t at = 0;
        while ((at = line.find('[', at)) != std::string::npos) {
            auto mid = line.find("](", at), end = mid == std::string::npos ? std::string::npos : line.find(')', mid + 2);
            if (end == std::string::npos) break;
            line.replace(at, end - at + 1, line.substr(at + 1, mid - at - 1));
            ++at;
        }
        for (std::size_t open; (open = line.find('<')) != std::string::npos;) {
            auto shut = line.find('>', open);
            if (shut == std::string::npos) break;
            line.erase(open, shut - open + 1);
        }
        for (auto token : {"**", "__", "`"}) {
            std::size_t pos = 0;
            while ((pos = line.find(token, pos)) != std::string::npos) line.erase(pos, std::strlen(token));
        }
        auto lead = line.find_first_not_of(' ');
        if (lead != std::string::npos && line.size() > lead + 1 && (line[lead] == '-' || line[lead] == '*') && line[lead + 1] == ' ')
            line = std::string(lead, ' ') + "  - " + line.substr(lead + 2);
        mc::text(line, mc::rgb(0xE0E0E0));
    }
}

// Underlined link text that opens a URL.
void linkText(char const* id, std::string const& title, std::string const& url) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 z = mc::measure(title);
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##link", z);
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    auto* dl = ImGui::GetWindowDrawList();
    ImU32 c  = hovered ? mc::rgb(0x9DBBFF) : mc::Link;
    mc::say(dl, p, c, title);
    dl->AddRectFilled({p.x, p.y + z.y}, {p.x + z.x, p.y + z.y + px()}, c);
    mc::tooltip(url);
    if (clicked) openUrl(url);
}

void details(ImVec2 a, ImVec2 b) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto* mod = selection();
    if (!mod) {
        mc::say(dl, {a.x, a.y + s(40)}, mc::Gray, i18n::tr("左の一覧からMODを選んでください。"));
        return;
    }
    // Header: icon, name with tags, version, author, configure button.
    float iconS = std::round(s(64)), side = mc::buttonHeight();
    drawIcon(dl, *mod, a, iconS);
    float tx = a.x + iconS + s(10), right = b.x - side - s(10);
    // Name + version, then author, then the chips on their own line.
    auto  name = mc::fit(mc::strong(), mod->name, (right - tx) * .7f);
    mc::say(dl, {tx, a.y - s(2)}, mc::White, name, mc::strong());
    if (!mod->version.empty()) {
        float vx = tx + mc::measure(name, mc::strong()).x + s(8);
        mc::say(dl, {vx, a.y - s(2)}, mc::Dim, mc::fit(mc::body(), mod->version, std::max(0.f, right - vx)));
    }
    float ly = a.y - s(2) + mc::body()->FontSize + s(2);
    mc::say(dl, {tx, ly}, mc::Dim, mc::fit(mc::body(), i18n::tr("作者: ") + (mod->author.empty() ? std::string(i18n::tr("不明")) : mod->author), right - tx));
    ly += mc::body()->FontSize + s(4);
    if (preferences.badges) drawTags(dl, tagsFor(*mod), {tx, ly}, right);

    ImGui::SetCursorScreenPos({b.x - side, a.y});
    bool configurable = !mod->configs.empty();
    if (configurable) {
        if (mc::glyphButton("configure", side, mc::sliders)) {
            openConfiguration(*mod);
        }
        mc::tooltip(i18n::tr("このMODの設定を開く"));
    }

    // Action buttons in one row of equal widths.
    float by = a.y + iconS + s(10);
    float gap = s(6), w = (b.x - a.x - gap * 2) / 3;
    Update status;
    bool   checking{};
    {
        std::lock_guard lock(updateMutex);
        if (auto it = updates.find(mod->id); it != updates.end()) status = it->second;
        checking = updating.contains(mod->id);
    }
    ImGui::SetCursorScreenPos({a.x, by});
    if (mod->self) mc::button("toggle", i18n::tr("このMODは常に有効"), {w, 0}, false);
    else if (mc::cycle("toggle", i18n::tr("状態"), mod->enabled ? i18n::tr("有効") : i18n::tr("無効"), mod->enabled ? mc::On : mc::Off, {w, 0})) askToggle = true;
    ImGui::SetCursorScreenPos({a.x + w + gap, by});
    if (mc::button("folder", i18n::tr("フォルダーを開く"), {w, 0})) openFolder(mod->folder);
    ImGui::SetCursorScreenPos({a.x + (w + gap) * 2, by});
    if (mc::button("update", checking ? i18n::tr("確認中...") : i18n::tr("更新を確認"), {w, 0}, !checking)) {
        auto copy = *mod;
        {
            std::lock_guard lock(updateMutex);
            updating.insert(mod->id);
        }
        std::thread([copy = std::move(copy)] {
            auto            update = checkUpdate(copy);
            std::lock_guard lock(updateMutex);
            updates[copy.id] = std::move(update);
            updating.erase(copy.id);
        }).detach();
    }

    // Scrolling information well.
    float top = by + mc::buttonHeight() + s(8);
    mc::well(dl, {a.x, top}, {b.x, b.y});
    ImGui::SetCursorScreenPos({a.x + s(10), top + s(8)});
    ImGui::BeginChild("info", {b.x - a.x - s(14), b.y - top - s(12)});
    static std::string shown;
    if (shown != selected) {
        ImGui::SetScrollY(0);
        shown = selected;
    }
    float wrap = ImGui::GetContentRegionAvail().x - s(4);
    mc::text(descriptionFor(*mod), mod->description.empty() ? mc::Dim : mc::White, nullptr, wrap);

    if (!status.version.empty()) {
        label(i18n::tr("更新:"));
        ImGui::Indent(s(12));
        mc::text(i18n::tr("公開中の最新版: ") + status.version, mc::On);
        mc::text(i18n::tr("対応するゲームのバージョンは配布元で確認してください。"), mc::Dim, nullptr, wrap - s(12));
        if (!status.url.empty()) linkText("release", i18n::tr("配布ページを開く"), status.url);
        ImGui::Unindent(s(12));
    } else if (!status.error.empty()) {
        label(i18n::tr("更新:"));
        ImGui::Indent(s(12));
        mc::text(status.error, mc::Dim, nullptr, wrap - s(12));
        ImGui::Unindent(s(12));
    }

    bool anyLink = std::any_of(mod->links.begin(), mod->links.end(), [](auto const& l) { return l.second.starts_with("http"); });
    if (anyLink) {
        label(i18n::tr("リンク:"));
        ImGui::Indent(s(12));
        for (auto const& [key, url] : mod->links)
            if (url.starts_with("https://") || url.starts_with("http://")) linkText(key.c_str(), linkLabel(key), url);
        ImGui::Unindent(s(12));
    }
    auto entry = [&](char const* title, std::string const& value) {
        if (value.empty()) return;
        label(title);
        ImGui::Indent(s(12));
        mc::text(value, mc::White, nullptr, wrap - s(12));
        ImGui::Unindent(s(12));
    };
    entry(i18n::tr("状態:"), std::string(!mod->enabled ? i18n::tr("無効") : (mod->loaded || mod->self) ? i18n::tr("ロード済み") : i18n::tr("未ロード")) + i18n::tr("（切り替えは再起動後に反映）"));
    entry(i18n::tr("ライセンス:"), mod->license);
    entry(i18n::tr("作者:"), mod->author);
    entry(i18n::tr("グループ:"), mod->parent);
    entry(i18n::tr("種類:"), mod->type);
    auto folderLabel=utf8(mod->folder.wstring());
#ifdef BE_PREVIEW
    wchar_t displayRoot[32768]{};
    if(GetEnvironmentVariableW(L"MODMENU_PREVIEW_DISPLAY_ROOT",displayRoot,32768))
        folderLabel=utf8(displayRoot)+"\\"+mod->id;
#endif
    entry(i18n::tr("フォルダー:"), folderLabel);
    if(!mod->dependencies.empty()) {
        label(i18n::tr("前提MOD:"));ImGui::Indent(s(12));
        for(auto const& dependency:mod->dependencies) {
            auto state=dependencyState(catalog,dependency);
            mc::text(dependency+(state==DependencyState::Missing?i18n::tr("  [未導入]"):state==DependencyState::Disabled?i18n::tr("  [無効]"):i18n::tr("  [有効]")),state==DependencyState::Enabled?mc::Gray:mc::Off,nullptr,wrap-s(12));
        }
        ImGui::Unindent(s(12));
    }
    if(fs::is_directory(mod->folder/L"logs")) {
        ImGui::Dummy({0,s(8)});
        if(mc::button("logs",i18n::tr("ログフォルダーを開く")))openFolder(mod->folder/L"logs");
    }
    if (!mod->readme.empty()) {
        ImGui::Dummy({0, s(10)});
        auto* cdl = ImGui::GetWindowDrawList();
        ImVec2 p  = ImGui::GetCursorScreenPos();
        cdl->AddRectFilled({p.x, p.y}, {p.x + wrap, p.y + px()}, mc::rgb(0xFFFFFF, 50));
        ImGui::Dummy({0, s(6)});
        mc::text("README", mc::Gray);
        ImGui::PushTextWrapPos(0);
        readme(mod->readme);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy({0, s(8)});
    ImGui::EndChild();
}

// ------------------------------------------------------------------ confirmation (a screen of its own)
void confirmDialog() {
    if (askToggle) {
        ImGui::OpenPopup("##confirm");
        askToggle = false;
    }
    auto* mod = selection();
    auto  vp  = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, mc::v4(mc::rgb(0x000000, 0)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground)) {
        auto*  dl = ImGui::GetWindowDrawList();
        ImVec2 c  = vp->GetCenter();
        dl->AddRectFilled(vp->Pos, {vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y}, mc::rgb(0x000000, 200));
        if (!mod) ImGui::CloseCurrentPopup();
        else {
            std::string title = mod->enabled ? mod->name + i18n::tr(" を無効にしますか？") : mod->name + i18n::tr(" を有効にしますか？");
            std::string body  = i18n::tr("変更はゲームを再起動すると反映されます。");
            ImVec2      tz    = mc::measure(title, mc::large()), bz = mc::measure(body);
            mc::say(dl, {c.x - tz.x * .5f, c.y - s(70)}, mc::White, title, mc::large());
            mc::say(dl, {c.x - bz.x * .5f, c.y - s(20)}, mc::Gray, body);
            float w = s(200);
            ImGui::SetCursorScreenPos({c.x - w - s(4), c.y + s(30)});
            if (mc::button("apply", mod->enabled ? i18n::tr("無効にする") : i18n::tr("有効にする"), {w, 0})) {
                changeEnabled(*mod, !mod->enabled, pendingChanges, notice);
                if (notice.empty()) notice = i18n::tr("変更を保存しました。ゲームを再起動すると反映されます。");
                refresh();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetCursorScreenPos({c.x + s(4), c.y + s(30)});
            if (mc::button("cancel", i18n::tr("キャンセル"), {w, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void leaveSubpage() {
    page = Page::Mods;
    documents.clear();
    pendingScale = -1;
}
} // namespace

// ------------------------------------------------------------------ public API
#ifdef BE_PREVIEW
bool previewRowCenter(std::string const& id, ImVec2& result) {
    auto it=previewRows.find(id);if(it==previewRows.end())return false;result=it->second;return true;
}
std::string previewSelectionState() {
    return std::string(page==Page::Config?"config|":"other|")+selected+"|"+(documents.empty()?"":utf8(documents.front().path.wstring()));
}
#endif
float fontScale(float height) { return std::clamp(height / 850.f, .72f, 1.7f) * float(preferences.scale); }
void  requestOpen(std::string, bool) { command.store(1); }
void  close() { command.store(3); }
void  toggle() { command.store(2); }
bool  isOpen() { return opened.load(); }
bool  needsFrame() { return opened.load() || command.load() != 0; }
bool  blocksInput() { return opened.load() || native::hasNativeSession(); }
void  abort() {
    opened  = false;
    command = 0;
    native::setOverlayAvailable(false);
}
void deviceLost() {}

void draw(float displayW, float displayH) {
    assets::apply(catalog);
    auto pending = command.exchange(0);
    bool active  = opened.load();
    if (pending == 1 || (pending == 2 && !active)) {
        if (!initialized) {
            preferences.load(settingsPath());
            i18n::select(preferences.language);
            selected    = preferences.selected;
            initialized = true;
        }
        refresh();
        page   = Page::Mods;
        active = true;
        notice.clear();
#ifdef BE_PREVIEW
        // Lets the preview tool capture a specific page.
        wchar_t previewPage[64]{}, previewMod[256]{};
        if (GetEnvironmentVariableW(L"MODMENU_PREVIEW_SELECT", previewMod, 256)) selected = utf8(previewMod);
        if (GetEnvironmentVariableW(L"MODMENU_PREVIEW_PAGE", previewPage, 64)) {
            std::wstring_view v(previewPage);
            if (v == L"prefs") page = Page::Preferences;
            else if (v == L"config" && selection()) {
                documents = openConfigs(*selection());
                page      = Page::Config;
            } else if (v == L"confirm") askToggle = true;
            else if(v==L"pending" && selection()){changeEnabled(*selection(),!selection()->enabled,pendingChanges,notice);refresh();}
            else if (v == L"filters") showFilters = true;
        }
#endif
    } else if (pending == 3 || (pending == 2 && active)) {
        active = false;
        persist();
    }
    opened = active;
    native::menuApplied(active);
    if (!active) return;

    unit = fontScale(displayH);
    ImGui::GetIO().FontGlobalScale = 1.f;
    mc::applyStyle();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (page != Page::Mods) leaveSubpage();
        else close();
    }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({displayW, displayH});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##BE", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                     | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    auto* dl = ImGui::GetWindowDrawList();
    // Backdrop: the game stays visible through a dark veil, a little darker at the edges.
#ifdef BE_PREVIEW
    if(!GetEnvironmentVariableW(L"MODMENU_PREVIEW_TRANSPARENT",nullptr,0))
#endif
    {
        dl->AddRectFilled({0, 0}, {displayW, displayH}, mc::rgb(0x000000, 140));
        dl->AddRectFilledMultiColor({0, 0}, {displayW, displayH * .18f}, mc::rgb(0, 70), mc::rgb(0, 70), mc::rgb(0, 0), mc::rgb(0, 0));
    }

    float  margin = std::max(s(24), (displayW - s(1400)) * .5f);
    ImVec2 min{margin, 0}, max{displayW - margin, displayH};
    float  headerH = s(54), footerH = s(64);

    // Title, centered over the content.
    std::string title = "ModMenu";
    if (page == Page::Config) {
        auto* mod = selection();
        title     = (mod ? mod->name : std::string("MOD")) + i18n::tr(" の設定");
    } else if (page == Page::Preferences) title = i18n::tr("メニューの設定");
    ImVec2 tz = mc::measure(title, mc::strong());
    mc::say(dl, {std::round((displayW - tz.x) * .5f), std::round((headerH - tz.y) * .5f + s(4))}, mc::White, title, mc::strong());
    if (page == Page::Mods) {
        std::string hint = i18n::tr("Esc で閉じる / MODをダブルクリックして設定");
        ImVec2      hz   = mc::measure(hint);
        mc::say(dl, {max.x - hz.x, std::round((headerH - hz.y) * .5f + s(4))}, mc::Dim, hint);
    }
    float side = mc::buttonHeight();
    ImGui::SetCursorScreenPos({max.x - side, std::round((headerH - side) * .5f + s(4))});
    if (page != Page::Mods) {
        if (mc::glyphButton("close", side, mc::cross)) leaveSubpage();
        mc::tooltip(i18n::tr("戻る (Esc)"));
    }

    ImVec2 bodyMin{min.x, headerH + s(8)}, bodyMax{max.x, displayH - footerH - (notice.empty() ? 0 : mc::body()->FontSize + s(10))};
    if(!pendingChanges.empty())bodyMax.y-=side+s(8);
    if (page == Page::Mods) {
        auto& io=ImGui::GetIO();
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F,false))focusSearch=true;
        auto visible = visibleMods();
        if(!io.WantTextInput && !ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId) && !visible.empty()) {
            auto current=std::find_if(visible.begin(),visible.end(),[](auto i){return catalog[i].id==selected;});
            int index=current==visible.end()?0:int(current-visible.begin());
            int delta=ImGui::IsKeyPressed(ImGuiKey_DownArrow)?1:ImGui::IsKeyPressed(ImGuiKey_UpArrow)?-1:0;
            if(delta){index=std::clamp(index+delta,0,int(visible.size())-1);selected=catalog[visible[index]].id;keyboardSelection=selected;persist();}
            if(ImGui::IsKeyPressed(ImGuiKey_Enter,false) && selection())openConfiguration(*selection());
        }
        if (!visible.empty() && std::none_of(visible.begin(), visible.end(), [](auto i) { return catalog[i].id == selected; })) selected = catalog[visible.front()].id;
        float gap   = s(28);
        float leftW = std::round((bodyMax.x - bodyMin.x - gap) * .47f);
        modList(bodyMin, {bodyMin.x + leftW, bodyMax.y});
        details({bodyMin.x + leftW + gap, bodyMin.y}, bodyMax);
    } else if (page == Page::Config) configPage(bodyMin, bodyMax);
    else preferencesPage(bodyMin, bodyMax);

    if(!pendingChanges.empty()) {
        float y=displayH-footerH-side-s(8);
        std::string summary=i18n::tr("再起動待ち: ");for(auto const& [id,change]:pendingChanges)summary+=change.mod.name+" ";
        mc::say(dl,{min.x,y+s(7)},mc::Gold,mc::fit(mc::body(),summary,max.x-min.x-s(180)));
        ImGui::SetCursorScreenPos({max.x-s(170),y});
        if(mc::button("undo-pending",i18n::tr("変更を取り消す"),{s(170),side})) {undoChanges(pendingChanges,notice);refresh();}
    }
    // Footer: centered row of buttons, notice above it.
    float fy = displayH - footerH + std::round((footerH - side) * .5f);
    std::vector<std::tuple<char const*, std::string, bool, std::function<void()>>> buttons;
    if (page == Page::Mods) {
        buttons.push_back({"updates",bulkChecking?i18n::tr("一括確認中..."):i18n::tr("すべて更新確認"),!bulkChecking,[]{checkAllUpdates();}});
        buttons.push_back({"modsdir", i18n::tr("MODフォルダーを開く"), true, [] { openFolder(directory().parent_path()); }});
        buttons.push_back({"prefs", i18n::tr("メニュー設定"), true, [] { page = Page::Preferences; }});
        buttons.push_back({"reload", i18n::tr("再読み込み"), true, [] {
                               refresh();
                               notice = i18n::tr("MODの一覧を再読み込みしました。");
                           }});
        buttons.push_back({"done", i18n::tr("完了"), true, [] { close(); }});
    } else if (page == Page::Config) {
        bool dirty = std::any_of(documents.begin(), documents.end(), [](auto const& d) { return d.dirty; });
        buttons.push_back({"cancel", i18n::tr("キャンセル"), true, [] {
                               leaveSubpage();
                               notice.clear();
                           }});
        buttons.push_back({"save", i18n::tr("保存して戻る"), dirty, [] {
                               if (saveConfigs(documents, notice)) leaveSubpage();
                           }});
    } else buttons.push_back({"back", i18n::tr("完了"), true, [] { leaveSubpage(); }});
    float bw    = std::round(s(page == Page::Mods ? 180 : 240));
    float total = buttons.size() * bw + (buttons.size() - 1) * s(8);
    float x     = std::round((displayW - total) * .5f);
    for (auto& [id, text, enabled, action] : buttons) {
        ImGui::SetCursorScreenPos({x, fy});
        if (mc::button(id, text, {bw, side}, enabled)) action();
        x += bw + s(8);
    }
    if (!notice.empty()) {
        ImVec2 nz = mc::measure(notice);
        mc::say(dl, {std::round((displayW - nz.x) * .5f), fy - nz.y - s(8)}, mc::rgb(0xFFFF55), notice);
    }

    confirmDialog();
    ImGui::End();
}
} // namespace be::ui
