#include "ModMenu/App.h"
#include "ModMenu/BridgeContracts.h"
#include "ModMenu/InputOwnership.h"
#include "ModMenu/I18n.h"
#include <array>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    using namespace be;
    int checks=0,failures=0;
    auto check=[&](bool ok,char const* name){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    check(i18n::normalize("zh-CN")=="zh_CN" && i18n::normalize("unknown")=="auto","language aliases normalized and invalid preference falls back to auto");
    i18n::select("ja_JP");check(std::string(i18n::tr("設定"))=="設定","Japanese UI preserves source text");
    i18n::select("en_US");check(std::string(i18n::tr("設定"))=="Settings","English UI translated");
    i18n::select("zh_CN");check(std::string(i18n::tr("設定"))=="设置","Simplified Chinese UI translated");
    check(std::string(i18n::tr("Example Mod"))=="Example Mod","third-party text left intact");
    i18n::observeGameLanguage("ja_JP");check(i18n::locale()=="zh_CN","manual preference overrides game language");
    i18n::select("auto");check(i18n::locale()=="ja_JP","auto follows observed game language");
    i18n::observeGameLanguage("zh_CN");check(i18n::locale()=="zh_CN","auto follows subsequent game language changes");
    i18n::observeGameLanguage("fr_FR");check(i18n::locale()=="en_US","unsupported game languages fall back to English");
    i18n::select("ja_JP");
    input::Ownership mouse;
    bool cycles=true;
    for(int i=0;i<10;++i) {
        cycles &= mouse.forward(1,true,false);
        cycles &= mouse.forward(1,false,true);
    }
    check(cycles,"ten native Mods clicks each retain their release across menu opening");
    check(!mouse.forward(1,true,true) && !mouse.forward(1,false,false),"menu-owned mouse release never leaks to game after closing");
    check(!mouse.forward(2,true,true) && !mouse.forward(2,false,true),"backing game buttons stay blocked while menu is open");
    input::Ownership keyboard;
    check(!keyboard.forward(27,true,true) && !keyboard.forward(27,false,false),"Escape press and release stay owned by closing menu");
    check(keyboard.forward(87,true,false) && !keyboard.forward(87,true,true) && keyboard.forward(87,false,true),"held game key can release while repeats and movement stay blocked");
    check(mouse.forward(1,true,false) && mouse.forward(2,true,false) && mouse.forward(1,false,true) && mouse.forward(2,false,true),"simultaneous game buttons each receive their matching release");
    auto root=fs::current_path()/L"build"/(L"test-fixtures-"+std::to_wstring(GetTickCount64()));
    auto put=[&](fs::path path,std::string const& text){fs::create_directories(path.parent_path());std::ofstream(path,std::ios::binary)<<text;};
    put(root/L"Alpha/manifest.json",R"({"name":"Alpha","version":"1.2.3","entry":"Alpha.dll","type":"native","author":"Alice","description":"HUD helpers"})");
    put(root/L"Alpha/config/options.json",R"({"enabled":true,"opacity":0.75,"count":9007199254740993,"nested":{"label":"test"},"list":[1,2]})");
    put(root/L"Alpha/modmenu.json",R"({"config":{"files":[{"path":"config/options.json"},{"path":"../outside.json"}]},"links":{"source":"https://example.com/project"}})");
    put(root/L"Beta/manifest.json.close",R"({"name":"Beta","version":"2","entry":"Beta.dll","authors":["Bob",{"name":"Carol"}]})");
    put(root/L"Library/manifest.json",R"({"name":"Library","entry":"lib.dll","modmenu":{"badges":["library"]}})");
    put(root/L"Broken/manifest.json","{");
    put(root/L"outside.json",R"({"doNotEdit":true})");
    put(root/L"Alpha/assets/icon.png","icon path fixture");
    auto mods=scan(root,root/L"Alpha");
    check(mods.size()==3,"malformed manifests do not prevent catalog discovery");
    auto alpha=std::find_if(mods.begin(),mods.end(),[](auto const& m){return m.id=="Alpha";});
    auto beta=std::find_if(mods.begin(),mods.end(),[](auto const& m){return m.id=="Beta";});
    check(alpha!=mods.end() && beta!=mods.end(),"enabled and disabled manifests discovered");
    if(alpha==mods.end() || beta==mods.end())return 1;
    check(alpha->self && alpha->loaded,"self reported as loaded");
    check(!beta->enabled && !beta->loaded,"disabled state independent of loaded state");
    check(beta->author=="Bob, Carol","author strings and author objects handled");
    check(alpha->configs.size()==1,"config metadata cannot escape mod directory");
    check(alpha->icon==root/L"Alpha/assets/icon.png","asset directory icon discovered without metadata");
    check(contained(root/L"Alpha",root/L"Alpha/config/options.json"),"contained config admitted");
    check(!contained(root/L"Alpha",root/L"Alpha2/x.json"),"sibling with common path prefix rejected");
    Preferences prefs;
    auto visible=filter(mods,"",prefs);check(visible.size()==2,"libraries filtered by default");
    check(filter(mods,"alpha HUD",prefs).size()==1,"case-insensitive multiple search terms");
    prefs.searchDescriptions=false;check(filter(mods,"HUD",prefs).empty(),"description search can be disabled");
    prefs.disabled=false;check(filter(mods,"",prefs).size()==1,"disabled visibility filter");
    prefs.disabled=true;prefs.libraries=true;prefs.reverse=true;
    visible=filter(mods,"",prefs);check(mods[visible.front()].name=="Library","descending alphabetical order");
    prefs.selected="Beta";prefs.save(root/L"preferences.json");Preferences loaded;loaded.load(root/L"preferences.json");
    check(loaded.reverse && loaded.libraries && loaded.selected=="Beta","preferences round trip");
    std::string error;check(!setEnabled(*alpha,false,error),"self cannot disable while active");
    check(setEnabled(*beta,true,error) && fs::exists(beta->folder/L"manifest.json"),"enabling renames manifest without deleting DLL");
    auto docs=openConfigs(*alpha);check(docs.size()==1 && docs[0].error.empty(),"JSON config opens");
    check(docs[0].draft["count"].get<std::uint64_t>()==9007199254740993ull,"large integer precision preserved");
    auto original=docs[0].original;docs[0].draft["enabled"]=false;docs[0].dirty=true;
    check(readFile(docs[0].path)==original,"draft edits never modify disk before Apply");
    check(saveConfigs(docs,error),"Apply writes config atomically");
    check(readFile(fs::path(docs[0].path.wstring()+L".bak"))==original,"backup preserves old config");
    check(!docs[0].dirty && Json::parse(readFile(docs[0].path))["enabled"]==false,"successful Apply resets draft state");
    docs[0].draft["opacity"]=.5;docs[0].dirty=true;put(docs[0].path,"{\"external\":true}");
    check(!saveConfigs(docs,error) && readFile(docs[0].path)=="{\"external\":true}","external edits cause conflict instead of overwrite");
    check(native::targetVersion(1,26,60,5) && !native::targetVersion(1,26,60,6),"native bridge exact build gate");
    std::array<std::byte,18> event{};unsigned kind=1,id=native::ModsEventId;
    std::memcpy(event.data(),&kind,4);std::memcpy(event.data()+8,&id,4);event[17]=std::byte{1};
    check(native::modsPress(event),"custom native Mods press recognized");
    event[16]=std::byte{1};check(!native::modsPress(event),"repeat press rejected");
    check(!native::modsPress(std::span(event).first(17)),"short native event rejected");
    if(argc==2) for(auto name:{"start_screen.json","pause_screen.json"}) {
        auto source=readFile(fs::path(wide(argv[1]))/name);std::string output;
        auto ns=name==std::string("start_screen.json")?"start":"pause";
        check(!source.empty() && native::InsertModsDefinition(source,output,ns),"actual vanilla menu accepts independently authored transform");
        if(output.empty())continue;
        auto before=Json::parse(source),after=Json::parse(output);
        if(ns==std::string("pause")) {
            check(after.at("be_mods_button@pause.settings_button").at("size")==before.at("pause_button_template@common_buttons.light_text_button").at("size"),"pause Mods retains native relative button size");
            Json const *originalRows=nullptr,*newRows=nullptr;
            for(auto const& row:before.at("menu_background@pause.transparent_background").at("controls"))if(row.contains("button_panel"))originalRows=&row.at("button_panel").at("controls");
            for(auto const& row:after.at("menu_background@pause.transparent_background").at("controls"))if(row.contains("button_panel"))newRows=&row.at("button_panel").at("controls");
            bool spaced=false;Json retained=Json::array();
            if(newRows)for(std::size_t i=0;i<newRows->size();++i) {
                auto const& row=(*newRows)[i];
                if(row.contains("be_mods@pause.be_mods_button"))spaced=i+1<newRows->size() && (*newRows)[i+1].contains("be_mods_padding@pause.vertical_padding") && (*newRows)[i+1].at("be_mods_padding@pause.vertical_padding").at("size")==Json::array({0,3});
                else if(!row.contains("be_mods_padding@pause.vertical_padding"))retained.push_back(row);
            }
            check(spaced,"pause Mods has native vertical spacing after its lower border");
            check(originalRows && retained==*originalRows,"pause layout preserves every original row and order");
        }
        check(before["namespace"]==after["namespace"] && output.find(native::ModsEventName)!=std::string::npos,"namespace retained and independent event inserted");
        std::string repeated;check(native::InsertModsDefinition(output,repeated,ns) && output==repeated,"native insertion is idempotent");
    }
    check(dependencyState(mods,"Alpha")==DependencyState::Enabled,"enabled dependency resolved");
    check(dependencyState(mods,"Beta")==DependencyState::Disabled,"disabled dependency reported");
    check(dependencyState(mods,"Missing")==DependencyState::Missing,"missing dependency reported");
    Mod tagged=*beta;tagged.type="preload-native";tagged.platform="client";
    Preferences tags;tags.searchDescriptions=false;
    check(filter({tagged},"PreLoader Client",tags).size()==1,"labels searchable without description search");
    check(filter({tagged},"Carol",tags).size()==1,"author search independent of descriptions");
    Preferences keys;check(keys.fallbackKey==0,"fallback key defaults off");check(keys.language=="auto","language defaults to follow game");keys.language="zh_CN";keys.fallbackKey=VK_F9;keys.updateFirst=true;keys.save(root/L"keys.json");
    Preferences restored;restored.load(root/L"keys.json");check(restored.fallbackKey==VK_F9 && restored.updateFirst,"fallback key and update ordering persist");
    check(restored.language=="zh_CN","manual display language persists across reload");
    put(root/L"keys.json",R"({"fallbackKey":4294967416})");restored.load(root/L"keys.json");check(restored.fallbackKey==0,"oversized key cannot wrap into an active hotkey");
    Mod pendingMod=*beta;pendingMod.id="Pending";pendingMod.folder=root/L"Pending";pendingMod.manifest=pendingMod.folder/L"manifest.json.close";
    put(pendingMod.manifest,R"({"name":"Pending"})");
    std::map<std::string,PendingChange> pending;std::string pendingMessage;
    check(changeEnabled(pendingMod,true,pending,pendingMessage) && pending.size()==1,"enable change tracked until restart");
    check(undoChanges(pending,pendingMessage) && pending.empty() && fs::is_regular_file(pendingMod.manifest),"undo restores original disabled manifest");
    check(changeEnabled(pendingMod,true,pending,pendingMessage),"repeat enable tracked");
    Mod nowEnabled=pendingMod;nowEnabled.enabled=true;nowEnabled.manifest=nowEnabled.folder/L"manifest.json";
    check(changeEnabled(nowEnabled,false,pending,pendingMessage) && pending.empty(),"return to original state removes restart pending entry");
    check(changeEnabled(pendingMod,true,pending,pendingMessage),"second enable tracked");
    put(pendingMod.folder/L"manifest.json",R"({"name":"ExternallyChanged"})");
    check(!undoChanges(pending,pendingMessage) && pending.size()==1 && fs::is_regular_file(pendingMod.folder/L"manifest.json"),"undo refuses externally edited manifests");
    std::string unchanged="keep";
    check(!native::InsertModsDefinition("{\"namespace\":\"start\",\"namespace\":\"start\"}",unchanged,"start") && unchanged=="keep","duplicate UI keys rejected without replacing original");
    check(!native::InsertModsDefinition("{\"namespace\":\"other\"}",unchanged,"start"),"unrelated UI rejected");
    std::cout<<checks<<" checks / "<<failures<<" failures\n";
    std::cout<<"Version: "<<MODMENU_VERSION<<"\n";
    return failures?1:0;
}
