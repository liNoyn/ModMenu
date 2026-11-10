#include "ModMenu/App.h"
#include "ModMenu/I18n.h"
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace be {
HMODULE module{};
std::string utf8(std::wstring_view value) {
    if(value.empty()) return {};
    int count=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),count,nullptr,nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if(value.empty()) return {};
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),count);
    return result;
}
fs::path directory() {
#ifdef BE_PREVIEW
    wchar_t preview[32768]{};
    if(GetEnvironmentVariableW(L"MODMENU_PREVIEW_ROOT",preview,32768))return preview;
#endif
    std::wstring path(32768,L'\0');
    auto count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if(!count || count>=path.size()) return fs::current_path();
    path.resize(count); return fs::path(path).parent_path();
}
std::string readFile(fs::path const& path) {
    std::error_code ec;
    auto size=fs::file_size(path,ec);
    if(ec || size>2*1024*1024) return {};
    std::ifstream file(path,std::ios::binary);
    return file ? std::string(std::istreambuf_iterator<char>(file),{}) : std::string{};
}
bool contained(fs::path const& base, fs::path const& child) {
    std::error_code ec;
    auto b=fs::weakly_canonical(base,ec); if(ec) return false;
    auto c=fs::weakly_canonical(child,ec); if(ec) return false;
    auto bi=b.begin(),ci=c.begin();
    for(;bi!=b.end();++bi,++ci) if(ci==c.end() || _wcsicmp(bi->c_str(),ci->c_str())!=0) return false;
    return true;
}
bool writeFile(fs::path const& path, std::string const& text, std::string& error) {
    fs::path temp=path;
    temp+=L".be-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) { error=i18n::tr("保存用ファイルを作成できません。"); return false; }
    DWORD written{};
    bool ok=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(ok) ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) { DeleteFileW(temp.c_str()); error=i18n::tr("保存できませんでした。元の設定は保持されています。"); }
    return ok;
}
namespace {
Json parse(std::string const& text) { return Json::parse(text,nullptr,false,true); }
std::string string(Json const& object,char const* key,std::string fallback={}) {
    auto it=object.find(key); return it!=object.end() && it->is_string() ? it->get<std::string>() : fallback;
}
std::string lower(std::string text) { for(char& c:text) if((unsigned char)c<128) c=char(std::tolower((unsigned char)c)); return text; }
bool boolean(Json const& j,char const* key,bool fallback) { auto i=j.find(key);return i!=j.end() && i->is_boolean()?i->get<bool>():fallback; }
void mergeMetadata(Mod& mod,Json const& data) {
    if(!data.is_object()) return;
    mod.metadata.merge_patch(data);
    mod.name=string(data,"displayName",mod.name);
    mod.description=string(data,"description",mod.description);
    mod.license=string(data,"license",mod.license);
    mod.parent=string(data,"parent",mod.parent);
    if(auto i=data.find("links");i!=data.end() && i->is_object())
        for(auto const& [key,value]:i->items()) if(value.is_string()) mod.links[key]=value.get<std::string>();
    for(auto key:{"website","source","issues"}) if(auto v=string(data,key);!v.empty()) mod.links[key]=v;
    if(auto i=data.find("badges");i!=data.end() && i->is_array())
        for(auto const& badge:*i) if(badge=="library") mod.library=true;
    if(auto icon=string(data,"icon");!icon.empty()) {
        auto p=mod.folder/fs::path(wide(icon));
        if(contained(mod.folder,p)) mod.icon=p;
    }
}
void configPaths(Mod& mod) {
    auto add=[&](fs::path p) {
        std::error_code ec;
        if(contained(mod.folder,p) && fs::is_regular_file(p,ec) && !ec && p.extension()==L".json") mod.configs.push_back(std::move(p));
    };
    auto config=mod.metadata.find("config");
    if(config!=mod.metadata.end() && config->is_boolean() && !config->get<bool>()) return;
    if(config!=mod.metadata.end() && config->is_object()) {
        auto files=config->find("files");
        if(files!=config->end() && files->is_array()) for(auto const& entry:*files)
            if(entry.is_object()) add(mod.folder/wide(string(entry,"path")));
    }
    if(mod.configs.empty()) {
        std::error_code ec;
        auto root=mod.folder/L"config";
        if(fs::is_directory(root,ec)) {
            for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;!ec && it!=end;it.increment(ec)) {
                if(it.depth()>2) { it.disable_recursion_pending(); continue; }
                if(mod.configs.size()>=32) break;
                add(it->path());
            }
        }
        for(auto const& name:{L"config.json",L"settings.json"}) add(mod.folder/name);
    }
    std::sort(mod.configs.begin(),mod.configs.end());
    mod.configs.erase(std::unique(mod.configs.begin(),mod.configs.end()),mod.configs.end());
}
std::string http(std::wstring const& host,std::wstring const& resource) {
    struct Handle { HINTERNET h; ~Handle(){if(h)WinHttpCloseHandle(h);} };
    Handle session{WinHttpOpen(L"ModMenu/" MODMENU_VERSION,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};
    if(!session.h) return {};
    WinHttpSetTimeouts(session.h,4000,4000,4000,6000);
    Handle connection{WinHttpConnect(session.h,host.c_str(),443,0)};
    if(!connection.h) return {};
    Handle request{WinHttpOpenRequest(connection.h,L"GET",resource.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.h || !WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.h,nullptr)) return {};
    DWORD status{},len=sizeof(status);
    if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&len,nullptr) || status!=200) return {};
    std::string result;
    for(;;) { char buffer[8192]; DWORD read{}; if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&read)) return {}; if(!read)break;result.append(buffer,read);if(result.size()>2*1024*1024)return {}; }
    return result;
}
}
void Preferences::load(fs::path const& path) {
    auto j=parse(readFile(path)); if(!j.is_object()) return;
    libraries=boolean(j,"showLibraries",libraries); disabled=boolean(j,"showDisabled",disabled); unloaded=boolean(j,"showUnloaded",unloaded);
    searchDescriptions=boolean(j,"searchDescriptions",searchDescriptions);badges=boolean(j,"showBadges",badges);reverse=boolean(j,"reverseSort",reverse);
    remember=boolean(j,"rememberSelection",remember);selected=string(j,"selected");
    updateFirst=boolean(j,"updatesFirst",updateFirst);
    language=i18n::normalize(string(j,"language","auto"));
    if(j.contains("fallbackKey") && j["fallbackKey"].is_number_integer()) {
        auto key=j["fallbackKey"].get<std::int64_t>();fallbackKey=key>=VK_F6 && key<=VK_F10?int(key):0;
    }
    if(fallbackKey<VK_F6 || fallbackKey>VK_F10)fallbackKey=0;
    if(j.contains("scale") && j["scale"].is_number()) scale=std::clamp(j["scale"].get<double>(),0.75,1.75);
}
void Preferences::save(fs::path const& path) const {
    Json j={{"showLibraries",libraries},{"showDisabled",disabled},{"showUnloaded",unloaded},{"searchDescriptions",searchDescriptions},
        {"showBadges",badges},{"reverseSort",reverse},{"rememberSelection",remember},{"selected",remember?selected:""},{"scale",scale},{"updatesFirst",updateFirst},{"fallbackKey",fallbackKey},{"language",i18n::normalize(language)}};
    std::error_code ec;fs::create_directories(path.parent_path(),ec);std::string error;writeFile(path,j.dump(2),error);
}
std::vector<Mod> scan(fs::path const& mods,fs::path const& self) {
    std::vector<Mod> result; std::error_code ec;
    for(fs::directory_iterator it(mods,fs::directory_options::skip_permission_denied,ec),end;!ec && it!=end;it.increment(ec)) {
        try {
            if(!it->is_directory()) continue;
            Mod mod;mod.folder=it->path();mod.id=utf8(mod.folder.filename().wstring());
            mod.manifest=mod.folder/L"manifest.json";mod.enabled=fs::is_regular_file(mod.manifest);
            if(!mod.enabled) mod.manifest=mod.folder/L"manifest.json.close";
            if(!fs::is_regular_file(mod.manifest)) continue;
            auto j=parse(readFile(mod.manifest));if(!j.is_object())continue;
            mod.name=string(j,"name",mod.id);mod.version=string(j,"version","?");mod.description=string(j,"description");mod.type=string(j,"type");
            mod.platform=string(j,"platform");
            if(auto d=j.find("dependencies");d!=j.end() && d->is_array()) for(auto const& dep:*d) {
                std::string name=dep.is_string()?dep.get<std::string>():dep.is_object()?string(dep,"name"):"";
                if(!name.empty()) mod.dependencies.push_back(name);
            }
            mod.author=string(j,"author");
            if(mod.author.empty() && j.contains("authors") && j["authors"].is_array()) for(auto const& a:j["authors"]) {
                std::string name=a.is_string()?a.get<std::string>():a.is_object()?string(a,"name"):"";
                if(!name.empty()) {if(!mod.author.empty())mod.author+=", ";mod.author+=name;}
            }
            mod.self=fs::equivalent(mod.folder,self,ec);ec.clear();
            auto entry=mod.folder/wide(string(j,"entry"));
            mod.loaded=mod.self || (contained(mod.folder,entry) && GetModuleHandleW(entry.c_str()));
            mod.icon=mod.folder/L"icon.png";
            if(j.contains("extraInfo")) mergeMetadata(mod,j["extraInfo"]);
            if(j.contains("modmenu")) mergeMetadata(mod,j["modmenu"]);
            auto metadata=parse(readFile(mod.folder/L"modmenu.json"));mergeMetadata(mod,metadata);
            if(!fs::is_regular_file(mod.icon,ec))for(auto const* candidate:{L"assets/icon.png",L"pack_icon.png",L"logo.png"}) {
                auto path=mod.folder/candidate;
                if(contained(mod.folder,path) && fs::is_regular_file(path,ec)){mod.icon=path;break;}
            }
            ec.clear();
            mod.library=mod.library || mod.id=="LeviLamina" || mod.id=="PreLoader";
            configPaths(mod);result.push_back(std::move(mod));
        } catch(...) { /* A broken manifest must not hide the rest of the catalog. */ }
        if(result.size()>=1024)break;
    }
    return result;
}
std::vector<std::size_t> filter(std::vector<Mod> const& mods,std::string query,Preferences const& prefs) {
    std::vector<std::size_t> indices;query=lower(std::move(query));
    for(std::size_t i=0;i<mods.size();++i) {
        auto const& mod=mods[i];
        if((!prefs.libraries && mod.library && !mod.self) || (!prefs.disabled && !mod.enabled) || (!prefs.unloaded && mod.enabled && !mod.loaded)) continue;
        auto text=lower(mod.name+" "+mod.id+" "+mod.author+" "+mod.type+" "+mod.platform+" "+
            (mod.type=="preload-native"?"PreLoader Preload":"LeviLamina")+" "+(mod.platform=="client"?"Client":"")+" "+
            (!mod.enabled?"Disabled 無効 ":"")+(mod.library?"Library ライブラリ ":"")+(prefs.searchDescriptions?" "+mod.description:""));
        std::istringstream words(query);std::string token;bool match=true;
        while(words>>token) if(text.find(token)==std::string::npos) {match=false;break;}
        if(match) indices.push_back(i);
    }
    std::stable_sort(indices.begin(),indices.end(),[&](auto a,auto b) {
        auto na=lower(mods[a].name),nb=lower(mods[b].name);
        return prefs.reverse?na>nb:na<nb;
    });
    return indices;
}
bool setEnabled(Mod const& mod,bool enabled,std::string& error) {
    if(mod.self) {error=i18n::tr("このメニューの無効化はランチャーから行ってください。");return false;}
    auto target=mod.folder/(enabled?L"manifest.json":L"manifest.json.close");
    if(!MoveFileExW(mod.manifest.c_str(),target.c_str(),0)) {error=i18n::tr("変更できません。同名ファイルやアクセス権を確認してください。");return false;}
    error=i18n::tr("変更しました。ゲームの再起動後に反映されます。");return true;
}
DependencyState dependencyState(std::vector<Mod> const& mods,std::string const& name) {
    auto key=lower(name);auto slash=key.find_last_of('/');auto tail=slash==std::string::npos?key:key.substr(slash+1);
    for(auto const& mod:mods)if(lower(mod.id)==key || lower(mod.name)==key || lower(mod.id)==tail || lower(mod.name)==tail)
        return mod.enabled?DependencyState::Enabled:DependencyState::Disabled;
    return DependencyState::Missing;
}
bool changeEnabled(Mod const& mod,bool desired,std::map<std::string,PendingChange>& pending,std::string& message) {
    if(mod.enabled==desired){message.clear();return true;}
    auto found=pending.find(mod.id);
    auto text=readFile(mod.manifest);
    if(found!=pending.end() && (found->second.desired!=mod.enabled || found->second.manifestText!=text)) {
        message=i18n::tr("外部でmanifestが変更されています。一覧とファイルを確認してください。");return false;
    }
    if(!setEnabled(mod,desired,message))return false;
    if(found==pending.end())pending.emplace(mod.id,PendingChange{mod,mod.enabled,desired,text});
    else if(found->second.original==desired)pending.erase(found);
    else found->second.desired=desired;
    return true;
}
bool undoChanges(std::map<std::string,PendingChange>& pending,std::string& message) {
    bool all=true;
    for(auto it=pending.begin();it!=pending.end();) {
        auto current=it->second.mod;current.enabled=it->second.desired;
        current.manifest=current.folder/(current.enabled?L"manifest.json":L"manifest.json.close");
        if(readFile(current.manifest)!=it->second.manifestText) {
            all=false;message=i18n::tr("外部でmanifestが変更されたMODは取り消しませんでした。");++it;continue;
        }
        if(!setEnabled(current,it->second.original,message)){all=false;++it;continue;}
        it=pending.erase(it);
    }
    message=all?i18n::tr("再起動待ちの変更を取り消しました。"):i18n::tr("外部変更などにより取り消せないMODが残っています。ファイルを確認してください。");
    return all;
}
std::vector<ConfigDocument> openConfigs(Mod const& mod) {
    std::vector<ConfigDocument> docs;
    for(auto const& path:mod.configs) {
        ConfigDocument d;d.path=path;d.original=readFile(path);d.draft=parse(d.original);
        if(d.draft.is_discarded())d.error=i18n::tr("JSONを読み取れません。ファイルを確認してください。");
        docs.push_back(std::move(d));
    }
    return docs;
}
bool saveConfigs(std::vector<ConfigDocument>& docs,std::string& message) {
    for(auto const& d:docs) if(d.dirty && readFile(d.path)!=d.original) {message=i18n::tr("外部で設定が変更されています。読み直してから保存してください。");return false;}
    for(auto& d:docs) if(d.dirty) {
        auto backup=d.path;backup+=L".bak";
        if(!CopyFileW(d.path.c_str(),backup.c_str(),FALSE)) {message=i18n::tr("設定のバックアップを作成できませんでした。");return false;}
        auto text=d.draft.dump(2)+"\n";
        if(!writeFile(d.path,text,message))return false;
        d.original=std::move(text);d.dirty=false;
    }
    message=i18n::tr("保存しました。設定の反映にはゲームの再起動が必要な場合があります。");return true;
}
Update checkUpdate(Mod const& mod) {
    Update update;std::string repo;
    if(mod.metadata.contains("update") && mod.metadata["update"].is_object()) repo=string(mod.metadata["update"],"github");
    if(repo.empty()) for(auto const& [key,url]:mod.links) if((key=="source" || key=="sources") && url.starts_with("https://github.com/")) {repo=url.substr(19);break;}
    if(repo.empty() || repo.find('/')==std::string::npos || repo.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._/")!=std::string::npos) {update.error=i18n::tr("更新元が登録されていません。");return update;}
    while(!repo.empty() && repo.back()=='/')repo.pop_back();
    auto j=parse(http(L"api.github.com",L"/repos/"+wide(repo)+L"/releases/latest"));
    if(!j.is_object()) {update.error=i18n::tr("更新を確認できませんでした。");return update;}
    update.version=string(j,"tag_name");update.url=string(j,"html_url");return update;
}
}
