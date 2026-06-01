// AutoPacer v14 - IAT Hook + Flip Upgrade + Waitable Swapchain
//
// WHY v13 FAILED:
//   1. 007 First Light uses DXGI_SWAP_EFFECT_SEQUENTIAL (BitBlt, SwapEffect=1).
//      v13 detected this and did nothing - waitable flag requires Flip model.
//   2. v13 created its own IDXGIFactory1 instance in InitThread, which raced
//      against DLSS Enabler's DXGI init at the same 1500ms window, causing
//      the black screen hang and game freeze.
//
// WHAT v14 DOES DIFFERENTLY:
//
//   A) IAT HOOK instead of factory instance:
//      We patch the Import Address Table of dxgi.dll's CreateDXGIFactory/1/2
//      exports. When ANY module (game, DLSS Enabler, OptiScaler) calls
//      CreateDXGIFactory*, they get our wrapper. We hook the resulting factory's
//      vtable once. No competing factory instance. No init race.
//
//   B) FLIP MODEL UPGRADE:
//      If the game requests DXGI_SWAP_EFFECT_SEQUENTIAL or FLIP_DISCARD
//      (BitBlt), we silently upgrade to DXGI_SWAP_EFFECT_FLIP_DISCARD.
//      This is exactly what Windows "Optimizations for windowed games" does.
//      Flip model is required for both VRR and the waitable object flag.
//
//   C) WAITABLE OBJECT (same concept as v13, now actually reachable):
//      With Flip model active, we inject FRAME_LATENCY_WAITABLE_OBJECT.
//      The handle fires when Intel's display pipeline has consumed the last
//      frame - this IS Intel's VBlank signal, not an approximation of it.
//      HookedPresent waits on it before each Present call.
//      ALLOW_TEARING is preserved throughout - VRR stays active.
//
// FLOW:
//   DllMain -> patch IAT for CreateDXGIFactory* in all loaded modules
//   Game calls CreateDXGIFactory -> our wrapper -> hook factory vtable
//   Game calls CreateSwapChain -> upgrade to Flip, add WAITABLE flag
//   Game calls Present -> wait on waitable object -> present
//   Result: frames only hit Intel's pipeline when Intel is ready for them

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

// ── Logging ───────────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_logCS;
static bool g_logCSInit = false;

static void Log(const char* msg)
{
    OutputDebugStringA("[AutoPacer v14] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    if (g_logCSInit) EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0)
    {
        fprintf(fp, "[AutoPacer v14] %s\n", msg);
        fclose(fp);
    }
    if (g_logCSInit) LeaveCriticalSection(&g_logCS);
}

static void Logf(const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    Log(buf);
}

// ── State ─────────────────────────────────────────────────────────────────────
static HANDLE        g_hWaitable      = nullptr;
static bool          g_WaitableActive = false;
static bool          g_FirstFrame     = true;
static std::atomic<bool> g_FactoryHooked{false};
static std::atomic<bool> g_PresentHooked{false};

static const UINT  FRAME_LATENCY      = 1;
static const DWORD WAITABLE_TIMEOUT   = 33; // ~30fps minimum timeout

// ── VTable patch ──────────────────────────────────────────────────────────────
static bool PatchVTable(void** vt, int slot, void* newFn, void** oldFn)
{
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;
    *oldFn = vt[slot];
    vt[slot] = newFn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

// ── Function pointer types ────────────────────────────────────────────────────
typedef HRESULT (WINAPI  *PFN_CreateDXGIFactory)    (REFIID, void**);
typedef HRESULT (WINAPI  *PFN_CreateDXGIFactory1)   (REFIID, void**);
typedef HRESULT (WINAPI  *PFN_CreateDXGIFactory2)   (UINT, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)     (IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSC)    (IDXGIFactory*, IUnknown*, HWND,
                                                       const DXGI_SWAP_CHAIN_DESC*,
                                                       IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCFHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                        const DXGI_SWAP_CHAIN_DESC1*,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                        IDXGIOutput*, IDXGISwapChain1**);

static PFN_CreateDXGIFactory  oCreateDXGIFactory  = nullptr;
static PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = nullptr;
static PFN_Present            oPresent            = nullptr;
static PFN_CreateSC           oCreateSC           = nullptr;
static PFN_CreateSCFHwnd      oCreateSCFHwnd      = nullptr;

// vtable slots
static const int SLOT_Present                = 8;
static const int SLOT_CreateSwapChain        = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;

// ── Waitable setup ────────────────────────────────────────────────────────────
static void SetupWaitable(IDXGISwapChain* sc)
{
    if (g_WaitableActive) return;

    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
    {
        HANDLE h = sc2->GetFrameLatencyWaitableObject();
        if (h)
        {
            HRESULT hr = sc2->SetMaximumFrameLatency(FRAME_LATENCY);
            Logf("SetMaximumFrameLatency(%u): hr=0x%08X", FRAME_LATENCY, (unsigned)hr);
            g_hWaitable      = h;
            g_WaitableActive = true;
            Log("Waitable object acquired - pipeline-sync ACTIVE");
        }
        else Log("WARNING: GetFrameLatencyWaitableObject returned null");
        sc2->Release();
    }
    else
        Log("WARNING: IDXGISwapChain2 not available - waitable mode unavailable");
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("First frame through Present hook");
        if (!g_WaitableActive) SetupWaitable(pSC);
        Logf("Waitable active: %s", g_WaitableActive ? "YES" : "NO");
        Beep(1000, 120);
    }

    if (g_WaitableActive && g_hWaitable)
    {
        DWORD r = WaitForSingleObjectEx(g_hWaitable, WAITABLE_TIMEOUT, FALSE);
        if (r == WAIT_TIMEOUT)
            Log("WARNING: Waitable timeout - presenting anyway");
    }

    return oPresent(pSC, SyncInterval, Flags);
}

// ── Hook Present on swapchain vtable ─────────────────────────────────────────
static void HookPresent(IDXGISwapChain* sc)
{
    if (g_PresentHooked.exchange(true)) return;
    void** vt = *(void***)sc;
    if (PatchVTable(vt, SLOT_Present, (void*)HookedPresent, (void**)&oPresent))
        Log("Present hook installed");
    else
    {
        g_PresentHooked = false;
        Log("ERROR: Present vtable patch failed");
    }
}

// ── SwapEffect name for logging ───────────────────────────────────────────────
static const char* SwapEffectName(DXGI_SWAP_EFFECT e)
{
    switch(e)
    {
        case DXGI_SWAP_EFFECT_DISCARD:          return "DISCARD(0)";
        case DXGI_SWAP_EFFECT_SEQUENTIAL:       return "SEQUENTIAL(1)";
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:  return "FLIP_SEQUENTIAL(3)";
        case DXGI_SWAP_EFFECT_FLIP_DISCARD:     return "FLIP_DISCARD(4)";
        default:                                return "UNKNOWN";
    }
}

// ── Hooked CreateSwapChain ────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    Logf("CreateSwapChain intercepted: SwapEffect=%s Flags=0x%X Buffers=%u",
        SwapEffectName(pDesc->SwapEffect), pDesc->Flags, pDesc->BufferCount);

    DXGI_SWAP_CHAIN_DESC desc = *pDesc;
    bool upgraded = false;

    // Upgrade BitBlt -> Flip. Required for waitable object and VRR.
    if (desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ||
        desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
    {
        desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        upgraded = true;
        Log("  Upgraded BitBlt -> FLIP_DISCARD");
    }

    bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                   desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);

    if (isFlip)
    {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Logf("  Added WAITABLE flag -> BufferCount=%u", desc.BufferCount);
    }

    HRESULT hr = oCreateSC(pFactory, pDevice, hWnd, &desc, ppSC);

    if (FAILED(hr) && upgraded)
    {
        // Flip upgrade failed - try original desc with just waitable
        Logf("  Flip upgrade failed (0x%08X), trying original SwapEffect + waitable", (unsigned)hr);
        DXGI_SWAP_CHAIN_DESC desc2 = *pDesc;
        desc2.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (desc2.BufferCount < 2) desc2.BufferCount = 2;
        hr = oCreateSC(pFactory, pDevice, hWnd, &desc2, ppSC);
    }

    if (FAILED(hr))
    {
        // Last resort: original call untouched
        Logf("  Waitable also failed (0x%08X), falling back to original", (unsigned)hr);
        hr = oCreateSC(pFactory, pDevice, hWnd, pDesc, ppSC);
    }

    if (SUCCEEDED(hr) && *ppSC)
    {
        Log("  CreateSwapChain succeeded");
        SetupWaitable(*ppSC);
        HookPresent(*ppSC);
    }
    else
        Logf("  CreateSwapChain FAILED: 0x%08X", (unsigned)hr);

    return hr;
}

// ── Hooked CreateSwapChainForHwnd ─────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSD,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    Logf("CreateSwapChainForHwnd intercepted: SwapEffect=%s Flags=0x%X Buffers=%u",
        SwapEffectName(pDesc->SwapEffect), pDesc->Flags, pDesc->BufferCount);

    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;
    bool upgraded = false;

    if (desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ||
        desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
    {
        desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        upgraded = true;
        Log("  Upgraded BitBlt -> FLIP_DISCARD");
    }

    bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                   desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);

    if (isFlip)
    {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Logf("  Added WAITABLE flag -> BufferCount=%u", desc.BufferCount);
    }

    HRESULT hr = oCreateSCFHwnd(pFactory, pDevice, hWnd, &desc, pFSD, pOutput, ppSC);

    if (FAILED(hr) && upgraded)
    {
        Logf("  Flip upgrade failed (0x%08X), trying original + waitable", (unsigned)hr);
        DXGI_SWAP_CHAIN_DESC1 desc2 = *pDesc;
        desc2.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (desc2.BufferCount < 2) desc2.BufferCount = 2;
        hr = oCreateSCFHwnd(pFactory, pDevice, hWnd, &desc2, pFSD, pOutput, ppSC);
    }

    if (FAILED(hr))
    {
        Logf("  Waitable also failed (0x%08X), falling back to original", (unsigned)hr);
        hr = oCreateSCFHwnd(pFactory, pDevice, hWnd, pDesc, pFSD, pOutput, ppSC);
    }

    if (SUCCEEDED(hr) && *ppSC)
    {
        Log("  CreateSwapChainForHwnd succeeded");
        SetupWaitable(*ppSC);
        HookPresent(*ppSC);
    }
    else
        Logf("  CreateSwapChainForHwnd FAILED: 0x%08X", (unsigned)hr);

    return hr;
}

// ── Hook factory vtable (called once we have any factory instance) ─────────────
static void HookFactoryVtable(IDXGIFactory* factory)
{
    if (g_FactoryHooked.exchange(true)) return;

    void** vt = *(void***)factory;

    if (PatchVTable(vt, SLOT_CreateSwapChain, (void*)HookedCreateSwapChain, (void**)&oCreateSC))
        Log("CreateSwapChain hook installed");
    else
        Log("ERROR: CreateSwapChain patch failed");

    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)))
    {
        void** vt2 = *(void***)f2;
        if (PatchVTable(vt2, SLOT_CreateSwapChainForHwnd,
            (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCFHwnd))
            Log("CreateSwapChainForHwnd hook installed");
        else
            Log("ERROR: CreateSwapChainForHwnd patch failed");
        f2->Release();
    }
}

// ── IAT-level wrappers for CreateDXGIFactory* ─────────────────────────────────
// These intercept factory creation from ANY module (game, DLSS Enabler, etc.)

static HRESULT WINAPI MyCreateDXGIFactory(REFIID riid, void** ppFactory)
{
    HRESULT hr = oCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && *ppFactory)
    {
        Log("CreateDXGIFactory called - hooking factory");
        HookFactoryVtable((IDXGIFactory*)*ppFactory);
    }
    return hr;
}

static HRESULT WINAPI MyCreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    HRESULT hr = oCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && *ppFactory)
    {
        Log("CreateDXGIFactory1 called - hooking factory");
        HookFactoryVtable((IDXGIFactory*)*ppFactory);
    }
    return hr;
}

static HRESULT WINAPI MyCreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    HRESULT hr = oCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && *ppFactory)
    {
        Log("CreateDXGIFactory2 called - hooking factory");
        HookFactoryVtable((IDXGIFactory*)*ppFactory);
    }
    return hr;
}

// ── IAT patcher ───────────────────────────────────────────────────────────────
// Walks the IAT of a module and replaces a named import with our function.
static bool PatchIAT(HMODULE hMod, const char* dllName, const char* funcName,
                     void* newFunc, void** oldFunc)
{
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD importRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return false;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importRVA);
    bool found = false;

    for (; imp->Name; ++imp)
    {
        const char* name = (const char*)(base + imp->Name);
        if (_stricmp(name, dllName) != 0) continue;

        IMAGE_THUNK_DATA* origThunk = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (int i = 0; thunk[i].u1.Function; ++i)
        {
            // Skip ordinal imports
            if (origThunk && IMAGE_SNAP_BY_ORDINAL(origThunk[i].u1.Ordinal)) continue;

            IMAGE_IMPORT_BY_NAME* ibn = origThunk
                ? (IMAGE_IMPORT_BY_NAME*)(base + origThunk[i].u1.AddressOfData)
                : nullptr;

            if (!ibn) continue;
            if (strcmp(ibn->Name, funcName) != 0) continue;

            DWORD old;
            void** target = (void**)&thunk[i].u1.Function;
            VirtualProtect(target, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            if (*oldFunc == nullptr) *oldFunc = *target;
            *target = newFunc;
            VirtualProtect(target, sizeof(void*), old, &old);
            found = true;
        }
    }
    return found;
}

// Patch IAT in all currently loaded modules
static void PatchAllModules()
{
    HMODULE mods[256];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return;

    DWORD count = min(needed / sizeof(HMODULE), 256u);

    // Get the real function pointers from dxgi.dll first
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) { Log("WARNING: dxgi.dll not loaded yet"); return; }

    if (!oCreateDXGIFactory)
        oCreateDXGIFactory  = (PFN_CreateDXGIFactory) GetProcAddress(dxgi, "CreateDXGIFactory");
    if (!oCreateDXGIFactory1)
        oCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!oCreateDXGIFactory2)
        oCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(dxgi, "CreateDXGIFactory2");

    int patched = 0;
    for (DWORD i = 0; i < count; ++i)
    {
        char modName[MAX_PATH] = {};
        GetModuleFileNameA(mods[i], modName, sizeof(modName));

        bool p = false;
        if (oCreateDXGIFactory)
            p |= PatchIAT(mods[i], "dxgi.dll", "CreateDXGIFactory",
                          (void*)MyCreateDXGIFactory,  (void**)&oCreateDXGIFactory);
        if (oCreateDXGIFactory1)
            p |= PatchIAT(mods[i], "dxgi.dll", "CreateDXGIFactory1",
                          (void*)MyCreateDXGIFactory1, (void**)&oCreateDXGIFactory1);
        if (oCreateDXGIFactory2)
            p |= PatchIAT(mods[i], "dxgi.dll", "CreateDXGIFactory2",
                          (void*)MyCreateDXGIFactory2, (void**)&oCreateDXGIFactory2);

        if (p)
        {
            Logf("IAT patched in: %s", modName);
            patched++;
        }
    }
    Logf("IAT patch complete: %d module(s) patched", patched);
}

// ── Init thread ───────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    // Wait for dxgi.dll to load - no Sleep(1500) needed, just poll
    for (int i = 0; i < 100; ++i)
    {
        if (GetModuleHandleA("dxgi.dll")) break;
        Sleep(50);
    }

    if (!GetModuleHandleA("dxgi.dll"))
    {
        Log("ERROR: dxgi.dll never loaded");
        return 1;
    }

    Log("dxgi.dll found - patching IAT");
    PatchAllModules();
    Log("Init complete. Waiting for swapchain creation...");
    Beep(880, 150);
    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logCS);
        g_logCSInit = true;
        Log("DLL loaded");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}
