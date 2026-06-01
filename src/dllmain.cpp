// AutoPacer v35 - The Mod-Safe Queue Shrinker
//
// Previous tests proved that the Mod's 6-Buffer swapchain creates a massive 
// GPU/PCIe reservoir that completely bypasses CPU pacing, causing clustering 
// and 0.000ms frame drops on the Intel iGPU.
//
// In v32, stripping the Waitable Object flag (0x40) crashed the mod. 
// Here, we preserve ALL flags (0x842) to keep the mod happy, but surgically 
// force the BufferCount down to 2 (matching the stock game) to physically 
// eliminate the deep queue clustering.

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
        fprintf(fp, "[AutoPacer v35] %s\n", msg);
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
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);

static PFN_CreateSC oCreateSC = nullptr;
static PFN_CreateSCForHwnd oCreateSCForHwnd = nullptr;
static PFN_Present oPresent = nullptr;

static const int SLOT_Present = 8;
static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;

// ── Hooked CreateSwapChain ────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, 
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    if (!pDesc) return oCreateSC(pFactory, pDevice, pDesc, ppSC);

    DXGI_SWAP_CHAIN_DESC newDesc = *pDesc;

    // If Mod requests a massive buffer queue (e.g., 6)
    if (newDesc.BufferCount > 3) {
        Logf("Intercepted CreateSwapChain! Mod requested Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        
        // Force buffer count to 2 (Stock config). PRESERVE ALL FLAGS so mod doesn't crash.
        newDesc.BufferCount = 2; 

        Logf("Forced Mod-Safe Config     -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        Beep(800, 150);
    }

    return oCreateSC(pFactory, pDevice, &newDesc, ppSC);
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSD,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    if (!pDesc) return oCreateSCForHwnd(pFactory, pDevice, hWnd, pDesc, pFSD, pOutput, ppSC);

    DXGI_SWAP_CHAIN_DESC1 newDesc = *pDesc;

    if (newDesc.BufferCount > 3) {
        Logf("Intercepted CreateSwapChainForHwnd! Mod requested Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        
        newDesc.BufferCount = 2;

        Logf("Forced Mod-Safe Config                      -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        Beep(800, 150);
    }

    return oCreateSCForHwnd(pFactory, pDevice, hWnd, &newDesc, pFSD, pOutput, ppSC);
}

// ── Hooked Present (Just for Telemetry Verification) ──────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        g_FirstFrame = false;
        
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pSC->GetDesc(&desc))) {
            Log("=================================================");
            Log("FIRST PRESENT VERIFICATION:");
            Logf("Actual BufferCount Running: %u", desc.BufferCount);
            Logf("Actual Flags Running      : 0x%X", desc.Flags);
            Log("=================================================");
        }
        Beep(1200, 200);
    }

    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init Thread (EAT Hook Factory & Present) ──────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) { if (GetModuleHandleA("dxgi.dll")) break; Sleep(50); }
    
    // 1. Hook the Factory (To catch CreateSwapChain)
    IDXGIFactory2* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&pFactory))) {
        void** vtable = *(void***)pFactory;
        WritePtr(&vtable[SLOT_CreateSwapChain], (void*)HookedCreateSwapChain, (void**)&oCreateSC);
        WritePtr(&vtable[SLOT_CreateSwapChainForHwnd], (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCForHwnd);
        pFactory->Release();
        Log("Factory hooks installed.");
    }

    // 2. Wait for game window
    HWND gameWnd = nullptr;
    for (int i = 0; i < 600; ++i) {
        Sleep(50);
        HWND fg = GetForegroundWindow();
        if (fg) {
            char title[256] = {}; GetWindowTextA(fg, title, sizeof(title));
            RECT r = {}; GetClientRect(fg, &r);
            if ((r.right - r.left) > 400 && title[0] != '\0') { gameWnd = fg; break; }
        }
    }
    Sleep(500);

    // 3. Hook Present (To verify it worked)
    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyWindow", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("DummyWindow", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pDummySC = nullptr; ID3D11Device* pDummyDev = nullptr; D3D_FEATURE_LEVEL featureLevel;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySC, &pDummyDev, &featureLevel, nullptr))) {
        void** vtable = *(void***)pDummySC;
        WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent);
        pDummySC->Release(); pDummyDev->Release();
    }
    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);

    Log("Waiting for Mod to create Swapchain...");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }
        remove(g_logPath);
        Log("DLL Booted - v35 Mod-Safe Queue Shrinker");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}