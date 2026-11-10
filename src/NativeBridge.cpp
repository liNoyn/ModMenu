#include "ModMenu/App.h"
#include "ModMenu/BridgeContracts.h"
#include "ModMenu/I18n.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <set>
#include <vector>

namespace be::native {
namespace {
std::atomic<bool> ready{false}, nativeSession{false};
std::atomic<bool> overlayAvailable{false};
std::atomic<bool> startAdded{},pauseAdded{};
std::mutex statusMutex;
std::string bridgeStatus="ボタンの接続を準備中";
void status(std::string value){std::lock_guard lock(statusMutex);bridgeStatus=std::move(value);}
using ParseText = bool (*)(void*, void const*, void*, bool);
using ParseLength = bool (*)(void*, char const*, std::uint64_t, void*, bool);
using ParseRange = bool (*)(void*, char const*, char const*, void*, bool);
using ReaderDtor = void (*)(void*);
using ControllerEvent = unsigned (*)(void*, void*);
using MainOpen = void (*)(void*);
using PauseCtor = void* (*)(void*, void*, bool);
ParseText originalText{};
ParseLength originalLength{};
ParseRange originalRange{};
ReaderDtor originalDtor{};
ControllerEvent originalEvent{};
MainOpen originalMainOpen{};
PauseCtor originalPauseCtor{};
using ChooseLanguage=void(*)(void*,std::string const&);
void* originalChooseLanguage{};
void chooseLanguage(void* self,std::string const& code) {
    reinterpret_cast<ChooseLanguage>(originalChooseLanguage)(self,code);
    i18n::observeGameLanguage(code);
    log::info("Game language: {}",code);
}
std::atomic<std::uintptr_t> mainTable{}, pauseTable{};
std::mutex buffersMutex;
// Json::Reader retains pointers for diagnostics after parse returns. Keep the
// replacement alive until that reader is destroyed, even through nested overloads.
std::unordered_map<void*, std::vector<std::shared_ptr<std::string const>>> buffers;
constexpr std::size_t maxDocument = 1024 * 1024;

bool readBytes(void const* p, void* out, std::size_t size) {
    SIZE_T copied{};
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, size, &copied) && copied == size;
}
template<class T> bool read(void const* p, T& value) { return readBytes(p, &value, sizeof(value)); }

std::string_view auditedDocument(std::string_view source) {
    // Fingerprints of the audited unmodified vanilla 1.26.60.05 ui.brarchive.
    // Custom resource-pack layouts are deliberately left unchanged.
    if (source.size() == 48127 && fingerprint(source) == 0xe3d0464100f7aa0dull) return "start";
    if (source.size() == 48802 && fingerprint(source) == 0x54cbb0117c68af89ull) return "pause";
    return {};
}
std::shared_ptr<std::string const> transform(void* reader, char const* input, std::size_t length) {
    if (!ready.load(std::memory_order_acquire) || length>maxDocument) return {};
    if(length!=48127 && length!=48802) {
        // Only diagnose a recognizable menu namespace; never transform a
        // resource-pack definition which differs from the audited documents.
        std::string prefix(std::min(length,std::size_t(512)),'\0');
        if(readBytes(input,prefix.data(),prefix.size()) && prefix.find("\"namespace\"")!=std::string::npos &&
           (prefix.find("\"start\"")!=std::string::npos || prefix.find("\"pause\"")!=std::string::npos))
            status("メニュー定義が確認済みサイズと異なるため、ボタンを挿入しませんでした。");
        return {};
    }
    std::string source(length, '\0');
    if (!readBytes(input, source.data(), length)) return {};
    auto scope = auditedDocument(source);
    if (scope.empty()) { status("メニュー定義が対象のバニラ定義と異なるため変更しませんでした。");return {}; }
    std::string output;
    if (!InsertModsDefinition(source, output, scope)) {
        log::warn("native {} definition refused; original menu retained", scope);
        return {};
    }
    auto owned = std::make_shared<std::string const>(std::move(output));
    {
        std::lock_guard lock(buffersMutex);
        // A reader may be reused for both menus. Earlier documents remain alive
        // while the original parser clears its previous diagnostic state.
        auto& retained = buffers[reader];
        if (retained.size() >= 128) return {}; // Bound unusually long-lived readers.
        retained.push_back(owned);
    }
    if(scope=="start")startAdded=true;else pauseAdded=true;
    log::info("native {} Mods button definition injected", scope);
    return owned;
}
bool parseText(void* reader, void const* document, void* root, bool comments) {
    std::shared_ptr<std::string const> patched;
    try {
        // std::string MSVC release x64 ABI; never allocate/free game-owned memory.
        struct StringLayout { char storage[16]; std::size_t size, capacity; } text{};
        if (read(document, text) && text.size <= maxDocument && text.capacity >= text.size) {
            char const* data = static_cast<char const*>(document);
            if (text.capacity >= 16) std::memcpy(&data, text.storage, sizeof(data));
            patched = transform(reader, data, text.size);
        }
    } catch (...) { log::warn("native string transform failed; original menu retained"); }
    return originalText(reader, patched ? patched.get() : document, root, comments);
}
bool parseLength(void* reader, char const* input, std::uint64_t size, void* root, bool comments) {
    std::shared_ptr<std::string const> patched;
    try {
        if (size <= maxDocument) patched = transform(reader, input, std::size_t(size));
    } catch (...) { log::warn("native length transform failed; original menu retained"); }
    return originalLength(reader, patched ? patched->data() : input, patched ? patched->size() : size, root, comments);
}
bool parseRange(void* reader, char const* begin, char const* end, void* root, bool comments) {
    std::shared_ptr<std::string const> patched;
    try {
        auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
        if (last >= first && last - first <= maxDocument)
            patched = transform(reader, begin, last - first);
    } catch (...) { log::warn("native range transform failed; original menu retained"); }
    return originalRange(reader, patched ? patched->data() : begin,
                         patched ? patched->data() + patched->size() : end, root, comments);
}
void readerDtor(void* reader) {
    // The original destructor can still access the replacement document.
    originalDtor(reader);
    std::lock_guard lock(buffersMutex);
    buffers.erase(reader);
}

bool executableTarget(void* target);
void observeController(void* controller, std::atomic<std::uintptr_t>& destination) {
    // The real client strips RTTI. Learn only a vtable reached through a
    // symbol-resolved, exact controller lifecycle method. No object survives.
    std::uintptr_t table{}, firstMethod{}; HMODULE owner{};
    MEMORY_BASIC_INFORMATION memory{};
    if (!read(controller, table) || !read(reinterpret_cast<void*>(table), firstMethod) ||
        !executableTarget(reinterpret_cast<void*>(firstMethod)) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(table), &owner) || owner != GetModuleHandleW(nullptr) ||
        VirtualQuery(reinterpret_cast<void*>(table), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return;
    destination.store(table, std::memory_order_release);
    log::info("Menu controller registered");
}
void mainOpen(void* controller) {
    originalMainOpen(controller);
    if (ready.load(std::memory_order_acquire)) observeController(controller, mainTable);
}
void* pauseCtor(void* controller, void* model, bool overlaySocialDrawerOnce) {
    auto result = originalPauseCtor(controller, model, overlaySocialDrawerOnce);
    if (ready.load(std::memory_order_acquire)) observeController(controller, pauseTable);
    return result;
}
Scene controllerScene(void* controller) {
    std::uintptr_t table{};
    if (!read(controller, table) || !table) return Scene::Unknown;
    if (table == mainTable.load(std::memory_order_acquire)) return Scene::Main;
    if (table == pauseTable.load(std::memory_order_acquire)) return Scene::Pause;
    return Scene::Unknown;
}
unsigned controllerEvent(void* controller, void* event) {
    try {
    if (ready.load(std::memory_order_acquire)) {
        std::array<std::byte, 18> bytes{};
        if (readBytes(event, bytes.data(), bytes.size())) {
            static std::mutex diagnosticMutex;
            static std::set<unsigned> diagnosticActions;
            unsigned kind{}, action{};
            std::memcpy(&kind,bytes.data(),4);std::memcpy(&action,bytes.data()+8,4);
            if(kind==1) {
                std::lock_guard lock(diagnosticMutex);
                if(diagnosticActions.size()<64 && diagnosticActions.insert(action).second)
                    log::info("Menu action {:08x}; custom {:08x}; scene {}",action,ModsEventId,int(controllerScene(controller)));
            }
            auto press = modsPress(bytes);

            if ((press || nativeSession.load()) && controllerScene(controller) != Scene::Unknown) {
                if (press && !overlayAvailable.load()) return 32;
                if (press && !nativeSession.exchange(true)) {
                    ui::requestOpen();
                    log::info("native Mods button opened ModMenu");
                    return 40; // DelayedFocusRefresh | ConsumeEvent; never Exit.
                }
                if (nativeSession.load()) return 32; // Keep underlying pause/main menu in place.
            }
        }
    }
    } catch (...) {
        nativeSession.store(false);
        log::warn("native event bridge failed; event passed through");
    }
    return originalEvent(controller, event);
}
bool executableTarget(void* target) {
    MEMORY_BASIC_INFORMATION memory{};
    return target && VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT &&
        !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
void* gameAddress(void* resolved) {
    // PreLoader's symbol handles are RIP-relative indirect jump thunks. Read
    // through them without invoking game code or scanning executable memory.
    for (unsigned i = 0; resolved && i < 4; ++i) {
        HMODULE owner{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(resolved), &owner) && owner == GetModuleHandleW(nullptr)) return resolved;
        std::array<unsigned char, 6> instruction{};
        if (!read(resolved, instruction) || instruction[0] != 0xff || instruction[1] != 0x25) return nullptr;
        std::int32_t displacement{}; std::memcpy(&displacement, instruction.data() + 2, 4);
        auto slot = reinterpret_cast<std::uintptr_t>(resolved) + 6 + displacement;
        if (!read(reinterpret_cast<void*>(slot), resolved)) return nullptr;
    }
    return nullptr;
}
void* rangeParserFromStringWrapper(void* resolved) {
    auto wrapper = static_cast<std::byte*>(gameAddress(resolved));
    if (!wrapper) return nullptr;
    // Audited 1.26.60.5 Reader::parse(string, Value&, bool): it computes R8
    // as begin+length, places the fifth bool on the stack, then calls the
    // unexported begin/end overload. No fixed game RVA is assumed.
    constexpr unsigned char tail[] = {0x4c,0x8d,0x04,0x3b,0x40,0x88,0x74,0x24,0x20,0x4c,0x8b,0xcd,0x49,0x8b,0xce,0xe8};
    std::array<unsigned char, sizeof(tail)> actual{};
    if (!readBytes(wrapper + 0x76, actual.data(), actual.size()) || std::memcmp(actual.data(), tail, sizeof(tail))) return nullptr;
    std::int32_t relative{};
    if (!read(wrapper + 0x86, relative)) return nullptr;
    auto target = wrapper + 0x8a + relative;
    if (gameAddress(target) != target || !executableTarget(target)) return nullptr;
    // Prologue is from the actual 1.26.60.05 client, independently verified
    // against the on-disk image before this fallback was added.
    constexpr unsigned char prologue[] = {
        0x40,0x53,0x55,0x56,0x57,0x48,0x81,0xec,0x88,0x00,0x00,0x00,
        0x48,0x8b,0x05,0x8d,0x1f,0x9c,0x03,0x48,0x33,0xc4,0x48,0x89,
        0x44,0x24,0x78,0x49,0x8b,0xf9,0x49,0x8b,0xe8,0x48,0x8b,0xf2,
        0x48,0x8b,0xd9,0x80,0x3a,0x00
    };
    std::array<unsigned char, sizeof(prologue)> entry{};
    if (!readBytes(target, entry.data(), entry.size()) || std::memcmp(entry.data(), prologue, sizeof(prologue))) return nullptr;
    return target;
}
void* internalEventFromWrapper(void* resolved) {
    auto wrapper = static_cast<std::byte*>(gameAddress(resolved));
    if (!wrapper) return nullptr;
    // Public handleEvent dispatches subcontrollers, then calls _handleEvent.
    // Some derived controllers call _handleEvent directly, so observe that
    // common implementation instead of only the public virtual wrapper.
    constexpr unsigned char tail[] = {0x89,0xc3,0x48,0x89,0xf9,0x48,0x89,0xf2,0xe8};
    std::array<unsigned char, sizeof(tail)> actual{};
    if (!readBytes(wrapper + 0x5d, actual.data(), actual.size()) || std::memcmp(actual.data(), tail, sizeof(tail))) return nullptr;
    std::int32_t relative{};
    if (!read(wrapper + 0x66, relative)) return nullptr;
    auto target = wrapper + 0x6a + relative;
    if (gameAddress(target) != target || !executableTarget(target)) return nullptr;
    constexpr unsigned char entry[] = {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00};
    std::array<unsigned char, sizeof(entry)> prologue{};
    if (!readBytes(target, prologue.data(), prologue.size()) || std::memcmp(prologue.data(), entry, sizeof(entry))) return nullptr;
    return target;
}
bool verifyVersion() {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, DWORD(std::size(path)))) return false;
    DWORD unused{}, bytes = GetFileVersionInfoSizeW(path, &unused);
    if (!bytes) return false;
    std::vector<std::byte> info(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, info.data())) return false;
    VS_FIXEDFILEINFO* version{}; UINT size{};
    if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&version), &size) || size < sizeof(*version)) return false;
    auto a = HIWORD(version->dwFileVersionMS), b = LOWORD(version->dwFileVersionMS);
    auto c = HIWORD(version->dwFileVersionLS), d = LOWORD(version->dwFileVersionLS);
    log::info("native bridge executable version {}.{}.{}.{}", a, b, c, d);
    return targetVersion(a, b, c, d);
}
}

bool installMenuEntry() {
    if (ready.load()) return true;
    if (!verifyVersion()) {
        status("このゲーム版は未対応です（対応: 1.26.60.5）。");
        log::warn("native menu integration inactive: expected 1.26.60.5");
        return false;
    }
    auto preloader = GetModuleHandleW(L"PreLoader.dll");
    using Resolve = void* (*)(char const*);
    auto resolve = preloader ? reinterpret_cast<Resolve>(GetProcAddress(preloader, "pl_resolve_symbol_silent")) : nullptr;
    using HookFn = int (*)(void*, void*, void**, int);
    using UnhookFn = bool (*)(void*, void*);
    auto attach = preloader ? reinterpret_cast<HookFn>(GetProcAddress(preloader, "pl_hook")) : nullptr;
    auto detach = preloader ? reinterpret_cast<UnhookFn>(GetProcAddress(preloader, "pl_unhook")) : nullptr;
    if (!resolve || !attach || !detach) { status("PreLoaderのシンボル解決・フック機能がありません。"); log::warn("native menu integration requires PreLoader symbol/hook APIs"); return false; }
    struct Hook { char const* symbol; void* detour; void** original; bool required; };
    Hook hooks[] = {
        {"??1Reader@Json@@QEAA@XZ", reinterpret_cast<void*>(readerDtor), reinterpret_cast<void**>(&originalDtor), true},
        {"?handleEvent@ScreenController@@UEAA?AW4ViewRequest@ui@@AEAUScreenEvent@@@Z", reinterpret_cast<void*>(controllerEvent), reinterpret_cast<void**>(&originalEvent), true},
        {"?onOpen@StartMenuScreenController@@UEAAXXZ", reinterpret_cast<void*>(mainOpen), reinterpret_cast<void**>(&originalMainOpen), true},
        {"??0PauseScreenController@@QEAA@V?$shared_ptr@VPauseScreenModel@@@std@@_N@Z", reinterpret_cast<void*>(pauseCtor), reinterpret_cast<void**>(&originalPauseCtor), true},
        {"?parse@Reader@Json@@QEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAVValue@2@_N@Z", reinterpret_cast<void*>(parseText), reinterpret_cast<void**>(&originalText), false},
        {"?parse@Reader@Json@@QEAA_NPEBD_KAEAVValue@2@_N@Z", reinterpret_cast<void*>(parseLength), reinterpret_cast<void**>(&originalLength), false},
        {"?parse@Reader@Json@@QEAA_NPEBD0AEAVValue@2@_N@Z", reinterpret_cast<void*>(parseRange), reinterpret_cast<void**>(&originalRange), false},
    };
    struct Resolved { Hook* hook; void* target; };
    std::vector<Resolved> found;
    bool missingRequired = false;
    unsigned parsers = 0;
    void* stringWrapper{};
    bool haveRange = false;
    for (auto& hook : hooks) {
        auto target = resolve(hook.symbol);
        if (!executableTarget(target)) {
            log::warn("native symbol unusable: {} (resolved {:p})", hook.symbol, target);
            if (hook.required) missingRequired = true;
            continue;
        }
        log::info("native symbol resolved: {} -> {:p}", hook.symbol, target);
        if (hook.original == reinterpret_cast<void**>(&originalEvent)) {
            if (auto internal = internalEventFromWrapper(target)) {
                log::info("native internal controller event implementation verified at {:p}", internal);
                target = internal;
            }
        }
        found.push_back({&hook, target});
        if (!hook.required) ++parsers;
        if (hook.original == reinterpret_cast<void**>(&originalText)) stringWrapper = target;
        if (hook.original == reinterpret_cast<void**>(&originalRange)) haveRange = true;
    }
    Hook rangeFallback{"audited Reader::parse(begin,end) call target", reinterpret_cast<void*>(parseRange), reinterpret_cast<void**>(&originalRange), false};
    if (!haveRange && stringWrapper) {
        if (auto target = rangeParserFromStringWrapper(stringWrapper)) {
            found.push_back({&rangeFallback, target});
            ++parsers;
            log::info("native unexported range parser verified at {:p}", target);
        } else log::warn("native unexported range parser could not be verified");
    }
    if (missingRequired || !parsers) { status("必要なゲームのシンボル／JSON解析処理が見つかりません。"); log::warn("native required symbols/parser missing; no hooks installed"); return false; }
    // pl_resolve_symbol can return PreLoader-owned generated executable thunks,
    // not addresses inside the original game module. Use its matching hook API;
    // hooking those thunks with an independent MinHook instance is insufficient.
    std::vector<Resolved> created;
    for (auto const& item : found) {
        int rc = attach(item.target, item.hook->detour, item.hook->original, 200);
        if (rc != 0) {
            for (auto it = created.rbegin(); it != created.rend(); ++it)
                detach(it->target, it->hook->detour);
            status("PreLoaderによる接続の設定に失敗しました。");
            log::warn("native PreLoader hook failed for {} (code {}); bridge inactive", item.hook->symbol, rc);
            return false;
        }
        created.push_back(item);
    }
    ready.store(true, std::memory_order_release);
    // Optional observation only: never changes the game's language. This
    // signature is declared by the headers for the exact supported build.
    auto languageTarget=resolve("?chooseLanguage@I18nImpl@@UEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    if(executableTarget(languageTarget) && attach(languageTarget,reinterpret_cast<void*>(chooseLanguage),&originalChooseLanguage,200)==0)
        log::info("Game language observer installed");
    else log::warn("Game language observer unavailable; automatic language uses Windows UI language");
    status("接続済み。確認済みのメニュー定義の読み込みを待っています。変更済み定義は挿入対象外です。");
    log::info("experimental native menu bridge installed ({} parser overloads); awaiting vanilla definitions", parsers);
    return true;
}
std::string menuStatus() {std::lock_guard lock(statusMutex);return std::string(i18n::tr("メイン画面: "))+i18n::tr(startAdded?"追加済み":"追加を確認できません")+i18n::tr(" / 一時停止: ")+i18n::tr(pauseAdded?"追加済み":"追加を確認できません")+"\n"+i18n::tr(startAdded && pauseAdded?"両方のメニューへの追加を確認しました。":bridgeStatus.c_str());}
#ifdef BE_PREVIEW
void previewButtonsReady(bool value){startAdded=value;pauseAdded=value;}
#endif
bool needsFallback(){return !startAdded.load() || !pauseAdded.load();}
bool hasNativeSession() { return nativeSession.load(); }
void menuApplied(bool open) { if (!open) nativeSession.store(false); }
void setOverlayAvailable(bool available) {
    overlayAvailable.store(available);
    if (!available) nativeSession.store(false);
}
}
