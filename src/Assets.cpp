#include "ModMenu/App.h"
#include <winhttp.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace be::assets {
namespace {
std::mutex mutex;
bool busy{};
std::map<std::string,Mod> completed;
std::string fold(std::string value) { for(auto& c:value)if((unsigned char)c<128)c=char(std::tolower((unsigned char)c));return value; }
std::string field(Json const& j,char const* key) {auto it=j.find(key);return it!=j.end() && it->is_string()?it->get<std::string>():"";}
std::string fetch(std::string const& url) {
#ifdef BE_PREVIEW
    if(GetEnvironmentVariableW(L"MODMENU_PREVIEW_OFFLINE",nullptr,0))return {};
#endif
    if(!url.starts_with("https://"))return {};
    auto address=wide(url);URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
    if(!WinHttpCrackUrl(address.c_str(),DWORD(address.size()),0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS || parts.dwUserNameLength || parts.dwPasswordLength)return {};
    struct H {HINTERNET h;~H(){if(h)WinHttpCloseHandle(h);}};
    H session{WinHttpOpen(L"ModMenu/" MODMENU_VERSION,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};if(!session.h)return {};
    WinHttpSetTimeouts(session.h,3000,3000,4000,5000);
    auto host=std::wstring(parts.lpszHostName,parts.dwHostNameLength),path=std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength);
    if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    H connection{WinHttpConnect(session.h,host.c_str(),parts.nPort,0)};if(!connection.h)return {};
    H request{WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.h || !WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0) || !WinHttpReceiveResponse(request.h,nullptr))return {};
    DWORD status{},length=sizeof(status);if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr) || status!=200)return {};
    std::string bytes;for(;;){char chunk[16384];DWORD count{};if(!WinHttpReadData(request.h,chunk,sizeof(chunk),&count))return {};if(!count)break;bytes.append(chunk,count);if(bytes.size()>4*1024*1024)return {};}
    return bytes;
}
std::wstring filename(std::string key) {key=fold(key);for(char& c:key)if(!std::isalnum((unsigned char)c) && c!='.' && c!='-')c='_';return wide(key)+L".img";}
}
void request(std::vector<Mod> const& mods) {
    {std::lock_guard lock(mutex);if(busy)return;busy=true;}
    std::thread([mods] {
        std::map<std::string,Mod> result;
        try {
            auto cache=directory()/L"cache";std::error_code ec;fs::create_directories(cache/L"icons",ec);
            auto old=directory().parent_path()/L"ModMenuBE"/L"cache";
            auto json=Json::parse(readFile(cache/L"catalog.json"),nullptr,false);
            if(!json.is_object())json=Json::parse(readFile(old/L"lip-index.json"),nullptr,false);
            // Existing on-disk metadata is enough for offline use. Refresh only
            // when absent or older than one day, entirely off the render thread.
            auto modified=fs::last_write_time(cache/L"catalog.json",ec);
            if(!json.is_object() || ec || fs::file_time_type::clock::now()-modified>std::chrono::hours(24)) {
                auto downloaded=fetch("https://levimc.org/mods/index.json");auto parsed=Json::parse(downloaded,nullptr,false);
                if(parsed.is_object() && parsed.contains("mods") && parsed["mods"].is_array()) {json=std::move(parsed);std::string error;writeFile(cache/L"catalog.json",downloaded,error);}
            }
            if(json.is_object() && json.contains("mods") && json["mods"].is_array())for(auto const& mod:mods) {
                // The product name can match an unrelated catalog project.
                // Keep this menu's own metadata and icon authoritative.
                if(mod.self)continue;
                Json const* found=nullptr;std::vector<Json const*> matches;
                for(auto const& entry:json["mods"]) {
                    auto tooth=fold(field(entry,"tooth"));auto name=fold(field(entry,"name"));
                    bool exact=false;for(auto const& [key,url]:mod.links)if(!tooth.empty() && fold(url).find(tooth)!=std::string::npos)exact=true;
                    if(exact){found=&entry;break;}
                    if(name==fold(mod.name) || name==fold(mod.id))matches.push_back(&entry);
                }
                if(!found && matches.size()==1)found=matches.front();
                if(!found) {
                    // An explicitly linked repository may have disappeared from
                    // the current catalog. A previously cached image still works.
                    for(auto const& [key,url]:mod.links)if(url.starts_with("https://github.com/")) {
                        auto target=cache/L"icons"/filename(url.substr(8));auto prior=old/L"icons"/filename(url.substr(8));
                        if(!fs::is_regular_file(mod.icon,ec) && fs::is_regular_file(prior,ec)) {
                            fs::copy_file(prior,target,fs::copy_options::skip_existing,ec);
                            if(fs::is_regular_file(target,ec)){Mod extra;extra.icon=target;result.emplace(mod.id,std::move(extra));}
                        }
                    }
                    continue;
                }
                Mod extra;extra.description=field(*found,"description");extra.license=field(*found,"license");
                auto tooth=field(*found,"tooth");if(tooth.starts_with("github.com/"))extra.links["source"]="https://"+tooth;
                auto page=field(*found,"url");if(!page.empty())extra.links["website"]=page;
                if(!fs::is_regular_file(mod.icon,ec)) {
                    auto target=cache/L"icons"/filename(tooth);
                    if(!fs::is_regular_file(target,ec)) {
                        auto prior=old/L"icons"/filename(tooth);
                        if(fs::is_regular_file(prior,ec))fs::copy_file(prior,target,fs::copy_options::skip_existing,ec);
                    }
                    if(!fs::is_regular_file(target,ec)) {
                        auto bytes=fetch(field(*found,"icon"));
                        if(!bytes.empty()){std::string error;writeFile(target,bytes,error);}
                    }
                    if(fs::is_regular_file(target,ec))extra.icon=target;
                }
                {std::lock_guard lock(mutex);completed.insert_or_assign(mod.id,extra);}
                if(tooth.starts_with("github.com/")) {
                    auto file=cache/(filename(tooth)+L".readme");
                    extra.readme=readFile(file);
                    if(extra.readme.empty()) {
                        extra.readme=fetch("https://raw.githubusercontent.com/"+tooth.substr(11)+"/HEAD/README.md");
                        if(extra.readme.empty())extra.readme=fetch("https://raw.githubusercontent.com/"+tooth.substr(11)+"/HEAD/readme.md");
                        if(extra.readme.size()>24000){std::size_t end=24000;while(end && ((unsigned char)extra.readme[end]&0xc0)==0x80)--end;extra.readme.resize(end);extra.readme+="\n\n続きはソースコードのリンクから確認できます。";}
                        if(!extra.readme.empty()){std::string error;writeFile(file,extra.readme,error);}
                    }
                }
                result.emplace(mod.id,std::move(extra));
            }
        }catch(std::exception const& error){log::warn("Catalog images unavailable: {}",error.what());}
        std::lock_guard lock(mutex);completed=std::move(result);busy=false;
    }).detach();
}
void apply(std::vector<Mod>& mods) {
    std::lock_guard lock(mutex);
    for(auto& mod:mods)if(auto it=completed.find(mod.id);it!=completed.end()) {
        auto const& extra=it->second;if(!extra.icon.empty())mod.icon=extra.icon;
        if(mod.description.empty())mod.description=extra.description;
        if(mod.license.empty())mod.license=extra.license;
        if(!extra.readme.empty())mod.readme=extra.readme;
        for(auto const& [key,url]:extra.links)mod.links.try_emplace(key,url);
    }
    completed.clear();
}
}
