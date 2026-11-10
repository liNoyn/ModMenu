#include "ModMenu/App.h"
#include "ModMenu/InputOwnership.h"
#include <fstream>
namespace be::log {
void write(char const* level,std::string const& text) {
    static std::mutex mutex;std::lock_guard lock(mutex);
    std::error_code ec;auto folder=directory()/L"logs";fs::create_directories(folder,ec);
    SYSTEMTIME now{};GetLocalTime(&now);
    std::ofstream file(folder/L"latest.log",std::ios::app);
    file<<std::format("{:02}:{:02}:{:02}.{:03} {} {}\n",now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,level,text);
}
}
namespace {
using MouseFeed=void(*)(void*,char,signed char,short,short,short,short,bool);
void* originalMouse{};
void mouseFeed(void* self,char button,signed char state,short x,short y,short dx,short dy,bool still) {
    static std::mutex mutex;
    static std::map<void*,be::input::Ownership> devices;
    bool blocked=be::ui::blocksInput(),forward=!blocked;
    if(button>=1 && button<=3 && (state==0 || state==1)) {
        std::lock_guard lock(mutex);
        forward=devices[self].forward(unsigned(button),state==1,blocked);
    }
    // Complete a game-owned press without passing camera motion through.
    if(forward)reinterpret_cast<MouseFeed>(originalMouse)(self,button,state,x,y,blocked?0:dx,blocked?0:dy,blocked?true:still);
}
DWORD WINAPI start(void*) {
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(start),&pinned);
    be::log::info("ModMenu {}: independent implementation", MODMENU_VERSION);
    try {
        be::native::installMenuEntry();
        auto pl=GetModuleHandleW(L"PreLoader.dll");
        if(pl) {
            using Resolve=void*(*)(char const*);using Hook=int(*)(void*,void*,void**,int);
            auto resolve=reinterpret_cast<Resolve>(GetProcAddress(pl,"pl_resolve_symbol_silent"));
            auto hook=reinterpret_cast<Hook>(GetProcAddress(pl,"pl_hook"));
            if(resolve && hook)if(auto target=resolve("?feed@MouseDevice@@QEAAXDCFFFF_N@Z"))hook(target,(void*)mouseFeed,&originalMouse,0);
        }
        for(int i=0;i<60;++i) {
            if(be::graphics::install()){be::log::info("Game menu and renderer hooks installed");return 0;}
            Sleep(500);
        }
        be::log::warn("Could not install graphics hooks");
    }catch(std::exception const& error){be::log::error("Startup failed: {}",error.what());}
    return 0;
}
}
extern "C" __declspec(dllexport) void ModMenu_Open() { be::ui::requestOpen(); }
extern "C" __declspec(dllexport) void ModMenu_Close() { be::ui::close(); }
extern "C" __declspec(dllexport) char const* ModMenu_Version() { return MODMENU_VERSION; }
// Compatibility exports for integrations targeting development builds.
extern "C" __declspec(dllexport) void BEMenu_Open() { ModMenu_Open(); }
extern "C" __declspec(dllexport) void BEMenu_Close() { ModMenu_Close(); }
extern "C" __declspec(dllexport) char const* BEMenu_Version() { return ModMenu_Version(); }
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        be::module=instance;DisableThreadLibraryCalls(instance);
        if(auto thread=CreateThread(nullptr,0,start,nullptr,0,nullptr))CloseHandle(thread);
    }
    return TRUE;
}
