#include "ModMenu/App.h"
#include "ModMenu/InputOwnership.h"
#include "ModMenu/I18n.h"
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace be::graphics {
namespace {
// Japanese plus punctuation, arrows and shapes used by the menu.
ImWchar const* glyphRanges(ImGuiIO& io) {
    static ImVector<ImWchar> ranges;
    if(ranges.empty()) {
        ImFontGlyphRangesBuilder builder;builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.AddText(i18n::glyphText().c_str());
        static ImWchar const extra[]={0x2000,0x206F,0x2190,0x21FF,0x25A0,0x25FF,0};
        builder.AddRanges(extra);builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}
template<class T> void release(T*& object) { if(object){object->Release();object=nullptr;} }
struct Buffer { ID3D11Resource* resource{};ID3D11RenderTargetView* view{}; };
std::vector<Buffer> surfaces;
ID3D11Device* device{};ID3D11DeviceContext* context{};ID3D11On12Device* bridge{};
IDXGISwapChain* activeChain{};
std::atomic<ID3D12CommandQueue*> commandQueue{};
std::recursive_mutex drawing;
HWND window{};WNDPROC previousProcedure{};
std::atomic<int> fallbackKey{};
bool backend{}, using12{};
std::map<fs::path,ID3D11ShaderResourceView*> pictures;
using Present=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,DXGI_PRESENT_PARAMETERS const*);
using Resize=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using Resize1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,UINT const*,IUnknown* const*);
using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
Present originalPresent{};Present1 originalPresent1{};Resize originalResize{};Resize1 originalResize1{};Execute originalExecute{};
decltype(&SetCursorPos) originalSetCursor{};decltype(&ClipCursor) originalClip{};
void clearSurfaces() {
    if(context && bridge)context->Flush();
    for(auto& surface:surfaces){release(surface.view);release(surface.resource);}surfaces.clear();
}
void clearDevice() {
    clearSurfaces();for(auto& [path,image]:pictures)release(image);pictures.clear();
    if(backend)ImGui_ImplDX11_Shutdown();backend=false;
    release(bridge);release(context);release(device);activeChain=nullptr;
}
LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(fallbackKey && w==WPARAM(fallbackKey.load()) && (msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN) && native::needsFallback()) {
        if(!(l&(1ll<<30)))ui::toggle();return 0;
    }
    if(msg==WM_KEYDOWN && w==VK_F11){std::lock_guard lock(drawing);clearSurfaces();}
    bool blocked=ui::blocksInput();
    static input::Ownership keys,buttons;
    bool paired=false,forward=false;
    if(msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN || msg==WM_KEYUP || msg==WM_SYSKEYUP) {
        paired=true;forward=keys.forward(unsigned(w),msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN,blocked);
    }
    unsigned button=0;bool down=false;
    switch(msg) {
    case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:button=1;down=true;break;
    case WM_LBUTTONUP:button=1;break;
    case WM_RBUTTONDOWN:case WM_RBUTTONDBLCLK:button=2;down=true;break;
    case WM_RBUTTONUP:button=2;break;
    case WM_MBUTTONDOWN:case WM_MBUTTONDBLCLK:button=3;down=true;break;
    case WM_MBUTTONUP:button=3;break;
    case WM_XBUTTONDOWN:case WM_XBUTTONDBLCLK:button=3+GET_XBUTTON_WPARAM(w);down=true;break;
    case WM_XBUTTONUP:button=3+GET_XBUTTON_WPARAM(w);break;
    }
    if(button){paired=true;forward=buttons.forward(button,down,blocked);}
    // Key/button releases and focus notifications must still reach ImGui
    // while closed: otherwise an Esc held during close stays down on reopen.
    bool release=msg==WM_KEYUP || msg==WM_SYSKEYUP || (button && !down);
    if(blocked || release || msg==WM_SETFOCUS || msg==WM_KILLFOCUS || msg==WM_CAPTURECHANGED) {
        // Handle input at event time: delayed keyboard processing loses Ctrl
        // state after a complete paste chord has already been released.
        {std::lock_guard lock(drawing);if(ImGui::GetCurrentContext())ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,l);}
    }
    if(msg==WM_SYSKEYDOWN && w==VK_F4)return CallWindowProcW(previousProcedure,hwnd,msg,w,l);
    if(paired && forward)return CallWindowProcW(previousProcedure,hwnd,msg,w,l);
    if(paired)return 0;
    if(blocked) {
        if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}
        if(msg==WM_INPUT)return DefWindowProcW(hwnd,msg,w,l);
        bool keyboard=msg>=WM_KEYFIRST && msg<=WM_KEYLAST;
        bool mouse=msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST;
        if(keyboard || mouse || msg==WM_CHAR)return 0;
    }
    return CallWindowProcW(previousProcedure,hwnd,msg,w,l);
}
void bindWindow(HWND target) {
    if(window==target || !target)return;
    if(window && previousProcedure)SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previousProcedure));
    if(ImGui::GetCurrentContext())ImGui_ImplWin32_Shutdown();
    else {
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    }
    window=target;previousProcedure=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(procedure)));
    ImGui_ImplWin32_Init(window);
}
bool initialize(IDXGISwapChain* chain) {
    if(activeChain && activeChain!=chain)clearDevice();
    if(backend)return true;
    ID3D12Device* d12{};
    using12=SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&d12)));
    if(using12) {
        auto queue=commandQueue.load();
        if(!queue){release(d12);return false;}
        IUnknown* queues[]={queue};
        auto hr=D3D11On12CreateDevice(d12,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,queues,1,0,&device,&context,nullptr);
        release(d12);
        if(FAILED(hr) || FAILED(device->QueryInterface(IID_PPV_ARGS(&bridge))))return false;
    } else {
        if(FAILED(chain->GetDevice(IID_PPV_ARGS(&device))))return false;
        device->GetImmediateContext(&context);
    }
    backend=ImGui_ImplDX11_Init(device,context);
    activeChain=chain;
    if(backend)log::info("Independent renderer ready ({})",using12?"D3D12":"D3D11");
    return backend;
}
bool makeSurfaces(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if(FAILED(chain->GetDesc(&description)))return false;
    unsigned count=using12?description.BufferCount:1;
    if(!count || count>16)return false;
    for(unsigned index=0;index<count;++index) {
        Buffer surface;
        if(using12) {
            ID3D12Resource* resource{};
            if(FAILED(chain->GetBuffer(index,IID_PPV_ARGS(&resource)))){clearSurfaces();return false;}
            D3D11_RESOURCE_FLAGS flags{};flags.BindFlags=D3D11_BIND_RENDER_TARGET;
            auto hr=bridge->CreateWrappedResource(resource,&flags,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT,IID_PPV_ARGS(&surface.resource));
            release(resource);
            if(FAILED(hr)){clearSurfaces();return false;}
        } else if(FAILED(chain->GetBuffer(index,IID_PPV_ARGS(&surface.resource))))return false;
        if(FAILED(device->CreateRenderTargetView(surface.resource,nullptr,&surface.view))) {release(surface.resource);clearSurfaces();return false;}
        surfaces.push_back(surface);
    }
    return true;
}
void frame(IDXGISwapChain* chain) {
    static thread_local HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    (void)com;
    std::lock_guard lock(drawing);
    DXGI_SWAP_CHAIN_DESC desc{};
    if(FAILED(chain->GetDesc(&desc)) || !IsWindow(desc.OutputWindow))return;
    bindWindow(desc.OutputWindow);
    static ULONGLONG lastSettingsRead{};
    if(GetTickCount64()-lastSettingsRead>1000){Preferences p;p.load(directory()/L"config"/L"menu.json");fallbackKey=p.fallbackKey;lastSettingsRead=GetTickCount64();}
    if(!ui::needsFrame()){clearSurfaces();return;}
    if(!initialize(chain)){ui::abort();log::warn("Renderer unavailable; input capture released");return;}
    if(surfaces.empty() && !makeSurfaces(chain)){ui::abort();return;}
    unsigned index=0;
    if(using12) {
        IDXGISwapChain3* version3{};
        if(FAILED(chain->QueryInterface(IID_PPV_ARGS(&version3)))){ui::abort();return;}
        index=version3->GetCurrentBackBufferIndex();release(version3);
    }
    if(index>=surfaces.size())return;
    // Rasterize for the actual display size instead of stretching a small
    // 17px atlas. Recreate only when the requested physical size changes.
    RECT client{};GetClientRect(window,&client);
    int pixels=int(std::round(18.f*ui::fontScale(float(client.bottom-client.top))));
    static int fontPixels{};
    if(fontPixels!=pixels) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        auto& io=ImGui::GetIO();io.Fonts->Clear();
        wchar_t system[MAX_PATH]{};GetWindowsDirectoryW(system,MAX_PATH);
        auto folder=fs::path(system)/L"Fonts";
        for(int role:{0,1,2}) {
            ImFontConfig options;options.OversampleH=3;options.OversampleV=2;options.PixelSnapH=false;
            float size=float(role==1?std::round(pixels*1.42f):pixels);
            auto latin=utf8((folder/(role?L"segoeuib.ttf":L"segoeui.ttf")).wstring());
            auto loaded=io.Fonts->AddFontFromFileTTF(latin.c_str(),size,&options);
            options.MergeMode=loaded!=nullptr;
            auto japanese=utf8((folder/L"meiryo.ttc").wstring());
            if(!io.Fonts->AddFontFromFileTTF(japanese.c_str(),size,&options,glyphRanges(io)) && !loaded)io.Fonts->AddFontDefault();
            options.MergeMode=true;
            auto chinese=utf8((folder/L"msyh.ttc").wstring());
            if(fs::is_regular_file(folder/L"msyh.ttc"))io.Fonts->AddFontFromFileTTF(chinese.c_str(),size,&options,glyphRanges(io));
        }
        io.Fonts->Build();fontPixels=pixels;
    }
    ImGui_ImplDX11_NewFrame();
    // Win32 already displays the native pointer. Drawing an additional
    // ImGui cursor here produces two overlapping pointers in the game.
    ImGui::GetIO().MouseDrawCursor=false;
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    auto size=ImGui::GetIO().DisplaySize;
    ui::draw(size.x,size.y);
    ImGui::Render();
    if(!ui::isOpen()) {
        auto& io=ImGui::GetIO();
        io.ClearEventsQueue();io.ClearInputKeys();io.ClearInputMouse();
        ImGui::SetWindowFocus(nullptr);
    }
    auto& surface=surfaces[index];
    if(using12)bridge->AcquireWrappedResources(&surface.resource,1);
    ID3D11RenderTargetView* savedTarget{};ID3D11DepthStencilView* savedDepth{};
    context->OMGetRenderTargets(1,&savedTarget,&savedDepth);
    context->OMSetRenderTargets(1,&surface.view,nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1,&savedTarget,savedDepth);release(savedTarget);release(savedDepth);
    if(using12){bridge->ReleaseWrappedResources(&surface.resource,1);context->Flush();}
    native::setOverlayAvailable(true);
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* chain,UINT sync,UINT flags) {
    if(!(flags&DXGI_PRESENT_TEST))try{frame(chain);}catch(...){ui::abort();}
    return originalPresent(chain,sync,flags);
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* chain,UINT sync,UINT flags,DXGI_PRESENT_PARAMETERS const* params) {
    if(!(flags&DXGI_PRESENT_TEST))try{frame(chain);}catch(...){ui::abort();}
    return originalPresent1(chain,sync,flags,params);
}
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* chain,UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags) {
    {std::lock_guard lock(drawing);clearSurfaces();}return originalResize(chain,count,w,h,format,flags);
}
HRESULT STDMETHODCALLTYPE resize1(IDXGISwapChain3* chain,UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags,UINT const* masks,IUnknown* const* queues) {
    {std::lock_guard lock(drawing);clearSurfaces();}return originalResize1(chain,count,w,h,format,flags,masks,queues);
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    if(!commandQueue.load() && queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT) {
        queue->AddRef();ID3D12CommandQueue* expected{};if(!commandQueue.compare_exchange_strong(expected,queue))queue->Release();
    }
    originalExecute(queue,count,lists);
}
BOOL WINAPI cursor(int x,int y){return ui::blocksInput()?TRUE:originalSetCursor(x,y);}
BOOL WINAPI clip(RECT const* rect){return originalClip(ui::blocksInput()?nullptr:rect);}
LRESULT CALLBACK dummyProcedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return DefWindowProcW(hwnd,msg,w,l);}
}
void* image(fs::path const& path) {
    if(!device || path.empty())return nullptr;
    if(auto it=pictures.find(path);it!=pictures.end())return it->second;
    ID3D11ShaderResourceView* view{};
    IWICImagingFactory* factory{};IWICBitmapDecoder* decoder{};IWICBitmapFrameDecode* source{};IWICFormatConverter* converter{};
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) {
        if(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)) && SUCCEEDED(decoder->GetFrame(0,&source)) && SUCCEEDED(factory->CreateFormatConverter(&converter))) {
            UINT width{},height{};source->GetSize(&width,&height);
            if(width && height && width<=2048 && height<=2048 && SUCCEEDED(converter->Initialize(source,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) {
                std::vector<unsigned char> pixels(std::size_t(width)*height*4);
                if(SUCCEEDED(converter->CopyPixels(nullptr,width*4,UINT(pixels.size()),pixels.data()))) {
                    D3D11_TEXTURE2D_DESC description{};description.Width=width;description.Height=height;description.MipLevels=description.ArraySize=1;description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.SampleDesc.Count=1;description.Usage=D3D11_USAGE_IMMUTABLE;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA initial{pixels.data(),width*4,0};ID3D11Texture2D* texture{};
                    if(SUCCEEDED(device->CreateTexture2D(&description,&initial,&texture))){device->CreateShaderResourceView(texture,nullptr,&view);release(texture);}
                }
            }
        }
    }
    release(converter);release(source);release(decoder);release(factory);pictures[path]=view;return view;
}
#ifdef BE_PREVIEW
void previewFrame(void* chain) { frame(static_cast<IDXGISwapChain*>(chain)); }
void previewResize() {std::lock_guard lock(drawing);clearSurfaces();}
void previewKey(int key){procedure(window,WM_KEYDOWN,WPARAM(key),0);procedure(window,WM_KEYUP,WPARAM(key),0);}
void previewMessage(UINT message,WPARAM w,LPARAM l){procedure(window,message,w,l);}
#endif
bool install() {
    auto status=MH_Initialize();if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED)return false;
    WNDCLASSW cls{};cls.lpfnWndProc=dummyProcedure;cls.hInstance=module;cls.lpszClassName=L"ModMenuGraphicsProbe";RegisterClassW(&cls);
    HWND dummy=CreateWindowW(cls.lpszClassName,L"",WS_POPUP,0,0,32,32,nullptr,nullptr,module,nullptr);
    if(!dummy)return false;
    ID3D12Device* d12{};ID3D12CommandQueue* queue{};IDXGIFactory4* factory{};IDXGISwapChain1* chain{};IDXGISwapChain3* chain3{};
    bool ok=false;std::vector<void*> created;
    auto attach=[&](void* target,void* detour,void** original) {
        if(MH_CreateHook(target,detour,original)!=MH_OK)return false;
        created.push_back(target);return true;
    };
    if(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12))) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(SUCCEEDED(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC1 d{};d.Width=d.Height=32;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if(SUCCEEDED(factory->CreateSwapChainForHwnd(queue,dummy,&d,nullptr,nullptr,&chain)) && SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
                auto methods=*reinterpret_cast<void***>(chain3);auto queues=*reinterpret_cast<void***>(queue);
                ok=attach(methods[8],(void*)present,(void**)&originalPresent) && attach(methods[13],(void*)resize,(void**)&originalResize) && attach(queues[10],(void*)execute,(void**)&originalExecute);
                if(ok){attach(methods[22],(void*)present1,(void**)&originalPresent1);attach(methods[39],(void*)resize1,(void**)&originalResize1);}
            }
        }
    }
    if(ok) {
        auto user=GetModuleHandleW(L"user32.dll");
        attach((void*)GetProcAddress(user,"SetCursorPos"),(void*)cursor,(void**)&originalSetCursor);
        attach((void*)GetProcAddress(user,"ClipCursor"),(void*)clip,(void**)&originalClip);
        for(auto target:created)if(MH_EnableHook(target)!=MH_OK){ok=false;break;}
    }
    if(!ok)for(auto target:created){MH_DisableHook(target);MH_RemoveHook(target);}
    release(chain3);release(chain);release(factory);release(queue);release(d12);DestroyWindow(dummy);
    if(ok)native::setOverlayAvailable(true);
    return ok;
}
}
