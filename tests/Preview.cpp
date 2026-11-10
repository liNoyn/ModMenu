#include "ModMenu/App.h"
#include "ModMenu/I18n.h"
#include <d3d11.h>
#include <wincodec.h>
#include <imgui.h>
namespace be::ui {bool previewRowCenter(std::string const&,ImVec2&);std::string previewSelectionState();}
namespace be::graphics {void previewKey(int);}
namespace be::graphics {void previewMessage(UINT,WPARAM,LPARAM);}
namespace be::native {void previewButtonsReady(bool);}
bool writePng(UINT width,UINT height,UINT pitch,BYTE* rgba,wchar_t const* path) {
    IWICImagingFactory* factory{};IWICStream* stream{};IWICBitmapEncoder* encoder{};IWICBitmapFrameEncode* frame{};
    HRESULT result=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(result))result=factory->CreateStream(&stream);
    if(SUCCEEDED(result))result=stream->InitializeFromFilename(path,GENERIC_WRITE);
    if(SUCCEEDED(result))result=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(result))result=encoder->Initialize(stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(result))result=encoder->CreateNewFrame(&frame,nullptr);
    if(SUCCEEDED(result))result=frame->Initialize(nullptr);
    if(SUCCEEDED(result))result=frame->SetSize(width,height);
    auto format=GUID_WICPixelFormat32bppRGBA;if(SUCCEEDED(result))result=frame->SetPixelFormat(&format);
    // PNG's encoder may negotiate BGRA even though the swap chain is RGBA.
    std::vector<BYTE> converted;
    auto* data=rgba;
    if(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)) {
        converted.assign(data,data+pitch*height);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)std::swap(converted[y*pitch+x*4],converted[y*pitch+x*4+2]);
        data=converted.data();
    }
    if(SUCCEEDED(result))result=frame->WritePixels(height,pitch,pitch*height,data);
    if(SUCCEEDED(result))result=frame->Commit();if(SUCCEEDED(result))result=encoder->Commit();
    if(frame)frame->Release();if(encoder)encoder->Release();if(stream)stream->Release();if(factory)factory->Release();
    return SUCCEEDED(result);
}
bool captureFrame(IDXGISwapChain* swap,ID3D11Device* device,ID3D11DeviceContext* context,wchar_t const* path) {
    ID3D11Texture2D *buffer{},*staging{};swap->GetBuffer(0,IID_PPV_ARGS(&buffer));
    D3D11_TEXTURE2D_DESC desc{};buffer->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging))){buffer->Release();return false;}
    context->CopyResource(staging,buffer);buffer->Release();D3D11_MAPPED_SUBRESOURCE pixels{};
    if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&pixels))){staging->Release();return false;}
    // The DX11 blend target contains premultiplied RGB. PNG expects straight
    // alpha; unpremultiply to avoid dark fringes on light README backgrounds.
    std::vector<BYTE> rgba(pixels.RowPitch*desc.Height);
    auto* source=static_cast<BYTE*>(pixels.pData);
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
        auto i=y*pixels.RowPitch+x*4;unsigned alpha=source[i+3];
        rgba[i+3]=BYTE(alpha);
        for(unsigned c=0;c<3;++c)rgba[i+c]=alpha?BYTE((std::min)(255u,(unsigned(source[i+c])*255u+alpha/2)/alpha)):0;
    }
    bool ok=writePng(desc.Width,desc.Height,pixels.RowPitch,rgba.data(),path);
    context->Unmap(staging,0);staging->Release();return ok;
}
namespace be::graphics {void previewFrame(void*);void previewResize();}
namespace be::log {void write(char const*,std::string const&) {}}
LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(window,message,w,l);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    bool transparent=GetEnvironmentVariableW(L"MODMENU_PREVIEW_TRANSPARENT",nullptr,0)>0;
    wchar_t capture[32768]{};bool snapshot=GetEnvironmentVariableW(L"MODMENU_PREVIEW_CAPTURE",capture,32768)>0;
    wchar_t clickMod[256]{};GetEnvironmentVariableW(L"MODMENU_PREVIEW_DOUBLECLICK",clickMod,256);int clickPhase=0;
    wchar_t keys[64]{};GetEnvironmentVariableW(L"MODMENU_PREVIEW_KEYS",keys,64);int keyPhase=0;
    wchar_t fallback[64]{};GetEnvironmentVariableW(L"MODMENU_PREVIEW_FALLBACK",fallback,64);int fallbackPhase=0;bool sawClosed=false;
    wchar_t lifecycle[8]{};GetEnvironmentVariableW(L"MODMENU_PREVIEW_LIFECYCLE",lifecycle,8);int lifecyclePhase=0;
    WNDCLASSW type{};type.hInstance=instance;type.lpszClassName=L"ModMenuPreview";type.lpfnWndProc=windowProc;RegisterClassW(&type);
    auto window=CreateWindowW(type.lpszClassName,L"ModMenu UI Preview",snapshot?WS_POPUP:WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,snapshot?1920:1400,snapshot?1080:900,nullptr,nullptr,instance,nullptr);
    DXGI_SWAP_CHAIN_DESC config{};config.BufferCount=2;config.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;config.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;config.OutputWindow=window;config.SampleDesc.Count=1;config.Windowed=TRUE;config.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap{};ID3D11Device* device{};ID3D11DeviceContext* context{};
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&config,&swap,&device,nullptr,&context)))return 1;
    ID3D11Texture2D* buffer{};ID3D11RenderTargetView* target{};swap->GetBuffer(0,IID_PPV_ARGS(&buffer));device->CreateRenderTargetView(buffer,nullptr,&target);buffer->Release();
    be::ui::requestOpen();MSG message{};auto started=GetTickCount64();
    while(message.message!=WM_QUIT) {
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        RECT client{};GetClientRect(window,&client);DXGI_SWAP_CHAIN_DESC current{};swap->GetDesc(&current);
        if(client.right>0 && client.bottom>0 && (UINT(client.right)!=current.BufferDesc.Width || UINT(client.bottom)!=current.BufferDesc.Height)) {
            context->OMSetRenderTargets(0,nullptr,nullptr);target->Release();be::graphics::previewResize();
            swap->ResizeBuffers(0,client.right,client.bottom,DXGI_FORMAT_UNKNOWN,0);
            swap->GetBuffer(0,IID_PPV_ARGS(&buffer));device->CreateRenderTargetView(buffer,nullptr,&target);buffer->Release();
        }
        float background[]={transparent?0.f:.1f,transparent?0.f:.14f,transparent?0.f:.12f,transparent?0.f:1.f};context->OMSetRenderTargets(1,&target,nullptr);context->ClearRenderTargetView(target,background);
        if(clickMod[0] && GetTickCount64()-started>1200 && clickPhase<4) {
            ImVec2 point;
            if(be::ui::previewRowCenter(be::utf8(clickMod),point)) {
                auto& io=ImGui::GetIO();io.AddMousePosEvent(point.x,point.y);io.AddMouseButtonEvent(0,clickPhase%2==0);++clickPhase;
            }
        }
        if(keys[0] && GetTickCount64()-started>1200 && keyPhase<4) {
            auto& io=ImGui::GetIO();
            if(std::wstring_view(keys)==L"ctrl-f") {
                if(keyPhase==0){io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_F,true);}
                if(keyPhase==2){io.AddKeyEvent(ImGuiKey_F,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);}
            } else io.AddKeyEvent(keyPhase<2?ImGuiKey_DownArrow:ImGuiKey_Enter,keyPhase%2==0);
            ++keyPhase;
        }
        if(fallback[0] && GetTickCount64()-started>1200 && fallbackPhase<2) {
            if(std::wstring_view(fallback)==L"blocked")be::native::previewButtonsReady(true);
            if(fallbackPhase==1)sawClosed=!be::ui::isOpen();
            be::graphics::previewKey(VK_F9);++fallbackPhase;
        }
        be::graphics::previewFrame(swap);
        if(lifecycle[0] && GetTickCount64()-started>1200 && lifecyclePhase<60) {
            int step=lifecyclePhase%12;
            if(step==0) {
                if(!be::ui::isOpen())return 7;
                ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);ImGui::GetIO().AddKeyEvent(ImGuiKey_F,true);
            }
            if(step==2){ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);ImGui::GetIO().AddKeyEvent(ImGuiKey_F,false);}
            if(step==4){if(!ImGui::GetIO().WantTextInput)return 8;be::graphics::previewMessage(WM_KEYDOWN,VK_ESCAPE,0);}
            if(step==7){if(be::ui::isOpen())return 9;be::graphics::previewMessage(WM_KEYUP,VK_ESCAPE,0);}
            if(step==8)be::ui::requestOpen();
            if(step==11 && !be::ui::isOpen())return 10;
            ++lifecyclePhase;
        }
        if(snapshot && GetTickCount64()-started>6000) {
            std::string missing;
            for(auto c:be::wide(be::i18n::glyphText()))if(c>=32)
                for(auto* font:ImGui::GetIO().Fonts->Fonts)if(!font->FindGlyphNoFallback(ImWchar(c))){missing+=be::utf8(std::wstring(1,c));break;}
            {std::string error;be::writeFile(std::wstring(capture)+L".glyphs",missing.empty()?"All translation glyphs found":missing,error);}
            if(!missing.empty())return 12;
            if(lifecycle[0] && lifecyclePhase!=60)return 11;
            if(fallback[0] && (sawClosed!=(std::wstring_view(fallback)==L"on") || !be::ui::isOpen()))return 6;
            if(keys[0]) {
                auto state=be::ui::previewSelectionState();std::string error;be::writeFile(std::wstring(capture)+L".result",state,error);
                if(std::wstring_view(keys)==L"ctrl-f") {if(!ImGui::GetIO().WantTextInput)return 4;}
                else if(!state.starts_with("config|Beta|"))return 5;
            }
            if(clickMod[0]) {
                auto expected="config|"+be::utf8(clickMod)+"|";auto actual=be::ui::previewSelectionState();
                std::string error;be::writeFile(std::wstring(capture)+L".result",actual,error);
                if(!actual.starts_with(expected))return 3;
            }
            return captureFrame(swap,device,context,capture)?0:2;
        }
        swap->Present(1,0);if(snapshot)Sleep(16);
    }
    return 0;
}
