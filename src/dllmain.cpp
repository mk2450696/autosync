// AutoPacer v36 - The Proxy Defeater
//
// In v35, the mod bypassed our CreateSwapChain hook by calling ResizeBuffers
// to pump the queue back to 6. This allowed the Nvidia GPU to cluster frames
// and cause the 0.000ms drops. 
// This version hooks ResizeBuffers and ResizeBuffers1 to create an inescapable
// net, forcefully locking the DXGI queue to 2 buffers to restore stock pacing.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <stdio.h>

static char g_logPath[MAX_PATH] = "AutoPacer.log";
static bool g_FirstFrame = true;

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v36] %s\n", msg);
        fclose(fp);
    }
}
static void Logf(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── VTable helpers ────────────────────────────────────────────────────────────
static bool WritePtr(void** addr, void* newVal, void** oldVal) {
    DWORD old;
    if (!VirtualProtect(addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (oldVal) *oldVal = *addr;
    *addr = newVal;
    VirtualProtect(addr, sizeof(void*), old, &old);
    return true;
}

// ── Function Pointers ─────────────────────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers1)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);

static PFN_CreateSC oCreateSC = nullptr;
static PFN_CreateSCForHwnd oCreateSCForHwnd = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;
static PFN_Present oPresent = nullptr;

static const int SLOT_Present = 8;
static const int SLOT_CreateSwapChain = 10;
static const int SLOT_ResizeBuffers = 13;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_ResizeBuffers1 = 39;

// ── Hooked Functions ──────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    if (!pDesc) return oCreateSC(pFactory, pDevice, pDesc, ppSC);
    DXGI_SWAP_CHAIN_DESC newDesc = *pDesc;
    if (newDesc.BufferCount > 3) {
        Logf("Intercepted CreateSwapChain! Requested Buffers: %u", newDesc.BufferCount);
        newDesc.BufferCount = 2; 
    }
    return oCreateSC(pFactory, pDevice, &newDesc, ppSC);
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSD,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    if (!pDesc) return oCreateSCForHwnd(pFactory, pDevice, hWnd, pDesc, pFSD, pOutput, ppSC);
    DXGI_SWAP_CHAIN_DESC1 newDesc = *pDesc;
    if (newDesc.BufferCount > 3) {
        Logf("Intercepted CreateSwapChainForHwnd! Requested Buffers: %u", newDesc.BufferCount);
        newDesc.BufferCount = 2;
    }
    return oCreateSCForHwnd(pFactory, pDevice, hWnd, &newDesc, pFSD, pOutput, ppSC);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pSC, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    UINT newCount = BufferCount;
    if (BufferCount > 3) {
        Logf("Intercepted ResizeBuffers! Mod tried to sneak %u buffers. Locking to 2.", BufferCount);
        newCount = 2;
        Beep(850, 150);
    }
    return oResizeBuffers(pSC, newCount, Width, Height, NewFormat, SwapChainFlags);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(
    IDXGISwapChain3* pSC, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, 
    UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    UINT newCount = BufferCount;
    if (BufferCount > 3) {
        Logf("Intercepted ResizeBuffers1! Mod tried to sneak %u buffers. Locking to 2.", BufferCount);
        newCount = 2;
        Beep(850, 150);
    }
    return oResizeBuffers1(pSC, newCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        g_FirstFrame = false;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pSC->GetDesc(&desc))) {
            Log("=================================================");
            Log("FIRST PRESENT VERIFICATION:");
            Logf("Actual BufferCount Running: %u", desc.BufferCount);
            Log("=================================================");
        }
        Beep(1200, 200);
    }
    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init Thread ───────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) { if (GetModuleHandleA("dxgi.dll")) break; Sleep(50); }
    
    // Hook Factory
    IDXGIFactory2* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&pFactory))) {
        void** vtable = *(void***)pFactory;
        WritePtr(&vtable[SLOT_CreateSwapChain], (void*)HookedCreateSwapChain, (void**)&oCreateSC);
        WritePtr(&vtable[SLOT_CreateSwapChainForHwnd], (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCForHwnd);
        pFactory->Release();
    }

    // Dummy Swapchain for Present & ResizeBuffers
    HWND dummyWnd = CreateWindowA("STATIC", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pDummySC = nullptr; ID3D11Device* pDummyDev = nullptr; D3D_FEATURE_LEVEL featureLevel;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySC, &pDummyDev, &featureLevel, nullptr))) {
        void** vtable = *(void***)pDummySC;
        WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent);
        WritePtr(&vtable[SLOT_ResizeBuffers], (void*)HookedResizeBuffers, (void**)&oResizeBuffers);
        
        IDXGISwapChain3* pSC3 = nullptr;
        if (SUCCEEDED(pDummySC->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&pSC3))) {
            void** vt3 = *(void***)pSC3;
            WritePtr(&vt3[SLOT_ResizeBuffers1], (void*)HookedResizeBuffers1, (void**)&oResizeBuffers1);
            pSC3->Release();
        }
        pDummySC->Release(); pDummyDev->Release();
        Log("Inescapable Hook Net Installed.");
    }
    DestroyWindow(dummyWnd);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v36 The Proxy Defeater");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}