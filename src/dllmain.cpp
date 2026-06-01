// AutoPacer v32 - The MPO Restorer (Memory Shift Fixed)
//
// Fixed the fatal C++ signature bug that caused the 1-billion buffer crash.
// Correctly intercepts the Mod's CreateSwapChain calls, strips the Waitable Object,
// and forces the BufferCount down to 2 so the Intel iGPU (which only has 2 MPO planes)
// can successfully engage Hardware Independent Flip and VRR.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <stdio.h>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v32] %s\n", msg);
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

// ── Function pointers (FIXED SIGNATURES) ──────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

static PFN_CreateSC oCreateSC = nullptr;
static PFN_CreateSCForHwnd oCreateSCForHwnd = nullptr;

static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;

// ── Hooked CreateSwapChain ────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, 
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    if (!pDesc) return oCreateSC(pFactory, pDevice, pDesc, ppSC);

    DXGI_SWAP_CHAIN_DESC newDesc = *pDesc;

    if (newDesc.BufferCount > 3 || (newDesc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        Logf("Intercepted CreateSwapChain! Original -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        
        newDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        newDesc.BufferCount = 2; 

        Logf("Restored Stock Config      -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        Beep(1000, 200);
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

    if (newDesc.BufferCount > 3 || (newDesc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        Logf("Intercepted CreateSwapChainForHwnd! Original -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        
        newDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        newDesc.BufferCount = 2;

        Logf("Restored Stock Config               -> Buffers: %u | Flags: 0x%X", newDesc.BufferCount, newDesc.Flags);
        Beep(1000, 200);
    }

    return oCreateSCForHwnd(pFactory, pDevice, hWnd, &newDesc, pFSD, pOutput, ppSC);
}

// ── Init Thread (EAT Hook Factory) ────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) { if (GetModuleHandleA("dxgi.dll")) break; Sleep(50); }
    
    IDXGIFactory2* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&pFactory))) {
        void** vtable = *(void***)pFactory;
        
        WritePtr(&vtable[SLOT_CreateSwapChain], (void*)HookedCreateSwapChain, (void**)&oCreateSC);
        WritePtr(&vtable[SLOT_CreateSwapChainForHwnd], (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCForHwnd);
        
        Log("Factory hooks installed. Waiting for Game/Mod to create Swapchain.");
        pFactory->Release();
    } else {
        Log("ERROR: Failed to create temp factory.");
    }
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
        Log("DLL Booted - v32 MPO Restorer (Memory Shift Fixed)");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}