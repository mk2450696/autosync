// AutoPacer v13 - Waitable Swapchain Injection for CASO iGPU+dGPU setups
//
// APPROACH: Hook IDXGIFactory::CreateSwapChain and CreateSwapChainForHwnd.
// When the game creates its swapchain, we add:
//   DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
// This gives us a handle synchronized to the display pipeline (Intel's VBlank
// on this system). A dedicated pacer thread waits on this handle before each
// Present, ensuring frames are only submitted when Intel's pipeline is ready.
//
// This is fundamentally different from all prior versions:
// - No external clock approximation (QPC, DWM timestamps)
// - No VBlank relay thread racing against Present
// - The waitable object IS Intel's pipeline signal
// - ALLOW_TEARING preserved -> VRR stays active
// - FG frames still generated normally; only their Present timing is gated
//
// FLOW:
//   1. Hook factory CreateSwapChain/CreateSwapChainForHwnd at DLL load
//   2. Game calls CreateSwapChain -> we add WAITABLE flag, intercept result
//   3. We call GetFrameLatencyWaitableObject() on the swapchain
//   4. We hook Present on the returned swapchain's vtable
//   5. HookedPresent: wait on waitable object (max 1 frame), then present
//   6. Waitable object signals when Intel's pipeline consumed the last frame
//   Result: every frame presented exactly when Intel is ready for it

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
#include <immintrin.h>

// ── Logging ───────────────────────────────────────────────────────────────────
static void Log(const char* msg)
{
    OutputDebugStringA(msg); OutputDebugStringA("\n");
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) { fprintf(fp, "%s\n", msg); fclose(fp); }
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── State ─────────────────────────────────────────────────────────────────────
static HANDLE g_hWaitableObject  = nullptr;  // from GetFrameLatencyWaitableObject
static bool   g_FirstFrame       = true;
static bool   g_WaitableActive   = false;    // true once we have a valid waitable SC

// Frame latency: 1 = minimum latency (present as soon as pipeline ready)
// If FG artifacts appear, try 2.
static const UINT FRAME_LATENCY = 1;

// Timeout for waitable wait: 1 full frame at 48Hz (lowest VRR) = ~21ms
static const DWORD WAITABLE_TIMEOUT_MS = 25;

// ── VTable patch ──────────────────────────────────────────────────────────────
static bool PatchVTable(void** vt, int slot, void* newFn, void** oldFn)
{
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *oldFn = vt[slot]; vt[slot] = newFn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

// ── Original function pointers ────────────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)            (IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSC)           (IDXGIFactory*,   IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCForHwnd)    (IDXGIFactory2*,  IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCForCoreWin) (IDXGIFactory2*,  IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

static PFN_Present          oPresent         = nullptr;
static PFN_CreateSC         oCreateSC        = nullptr;
static PFN_CreateSCForHwnd  oCreateSCForHwnd = nullptr;

// Factory vtable slots (IDXGIFactory)
// CreateSwapChain is slot 10 on IDXGIFactory
// IDXGIFactory2: CreateSwapChainForHwnd is slot 15
static const int SLOT_CreateSwapChain        = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_Present                = 8;

// ── Setup waitable object from a swapchain ────────────────────────────────────
static void SetupWaitable(IDXGISwapChain* sc)
{
    if (g_WaitableActive) return; // already set up

    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
    {
        HANDLE h = sc2->GetFrameLatencyWaitableObject();
        if (h)
        {
            // Set frame latency to 1 for minimum pipeline depth
            HRESULT hr = sc2->SetMaximumFrameLatency(FRAME_LATENCY);
            Logf("[AutoPacer v13] SetMaximumFrameLatency(%u): 0x%08X", FRAME_LATENCY, (unsigned)hr);

            g_hWaitableObject = h;
            g_WaitableActive  = true;
            Log("[AutoPacer v13] Waitable object acquired - pipeline-synchronized mode ACTIVE");
        }
        else Log("[AutoPacer v13] WARNING: GetFrameLatencyWaitableObject returned null");
        sc2->Release();
    }
    else
    {
        // Swapchain doesn't support IDXGISwapChain2 - wasn't created with waitable flag
        // This happens if the game uses DX12 or if our flag injection didn't work
        Log("[AutoPacer v13] WARNING: IDXGISwapChain2 not available on game swapchain");
        Log("[AutoPacer v13] Waitable mode unavailable - falling back to pass-through");
    }
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("[AutoPacer v13] First frame - hook confirmed active");
        // Try to get waitable object if not already set up
        // (in case the game's swapchain was created before our hook on CreateSwapChain fired)
        if (!g_WaitableActive) SetupWaitable(pSC);
        Logf("[AutoPacer v13] Waitable active: %s", g_WaitableActive ? "YES" : "NO");
        Beep(1000, 120);
    }

    // Wait for Intel's pipeline to be ready for the next frame.
    // This is the core mechanism: the waitable object fires when the display
    // pipeline (Intel CASO path) has consumed the previous frame and is ready.
    // With VRR active, this fires at whatever rate the display is running at.
    if (g_WaitableActive && g_hWaitableObject)
    {
        DWORD waitResult = WaitForSingleObjectEx(g_hWaitableObject, WAITABLE_TIMEOUT_MS, FALSE);
        if (waitResult == WAIT_TIMEOUT)
        {
            // Timeout - pipeline may be stalled. Present anyway to avoid deadlock.
            Log("[AutoPacer v13] WARNING: Waitable timeout - presenting anyway");
        }
    }

    // Pass everything through unchanged. ALLOW_TEARING preserved. VRR active.
    return oPresent(pSC, SyncInterval, Flags);
}

// ── Hook Present on a swapchain ───────────────────────────────────────────────
static void HookPresentOnSwapchain(IDXGISwapChain* sc)
{
    if (oPresent) return; // already hooked
    void** vt = *(void***)sc;
    if (PatchVTable(vt, SLOT_Present, (void*)HookedPresent, (void**)&oPresent))
        Log("[AutoPacer v13] Present hook installed");
    else
        Log("[AutoPacer v13] ERROR: Present vtable patch failed");
}

// ── Hooked CreateSwapChain (IDXGIFactory, DX11 legacy path) ──────────────────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    Log("[AutoPacer v13] CreateSwapChain intercepted");

    DXGI_SWAP_CHAIN_DESC desc = *pDesc;

    // Only add waitable flag for flip model swapchains
    // (DXGI_SWAP_EFFECT_FLIP_DISCARD or FLIP_SEQUENTIAL)
    bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                   desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);

    if (isFlip)
    {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        // Need at least 2 buffers for waitable
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        Logf("[AutoPacer v13] Added WAITABLE flag (flip=%d, buffers=%u)", isFlip, desc.BufferCount);
    }
    else
    {
        Logf("[AutoPacer v13] Non-flip swapchain (SwapEffect=%u) - no waitable flag", desc.SwapEffect);
    }

    HRESULT hr = oCreateSC(pFactory, pDevice, hWnd, &desc, ppSwapChain);

    if (SUCCEEDED(hr) && *ppSwapChain)
    {
        Logf("[AutoPacer v13] CreateSwapChain succeeded");
        if (isFlip) SetupWaitable(*ppSwapChain);
        HookPresentOnSwapchain(*ppSwapChain);
    }
    else
    {
        // If waitable flag caused failure, retry without it
        if (isFlip && FAILED(hr))
        {
            Logf("[AutoPacer v13] Failed with waitable (0x%08X), retrying without...", (unsigned)hr);
            hr = oCreateSC(pFactory, pDevice, hWnd, pDesc, ppSwapChain);
            if (SUCCEEDED(hr) && *ppSwapChain)
            {
                Log("[AutoPacer v13] CreateSwapChain succeeded without waitable flag");
                HookPresentOnSwapchain(*ppSwapChain);
            }
        }
    }

    return hr;
}

// ── Hooked CreateSwapChainForHwnd (IDXGIFactory2, DX11/DX12 modern path) ─────
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    Log("[AutoPacer v13] CreateSwapChainForHwnd intercepted");

    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                   desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);

    if (isFlip)
    {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        Logf("[AutoPacer v13] Added WAITABLE flag (flip=%d, buffers=%u, format=%u)",
            isFlip, desc.BufferCount, desc.Format);
    }

    HRESULT hr = oCreateSCForHwnd(pFactory, pDevice, hWnd, &desc,
                                   pFullscreenDesc, pRestrictToOutput, ppSwapChain);

    if (SUCCEEDED(hr) && *ppSwapChain)
    {
        Logf("[AutoPacer v13] CreateSwapChainForHwnd succeeded");
        if (isFlip) SetupWaitable(*ppSwapChain);
        HookPresentOnSwapchain(*ppSwapChain);
    }
    else if (isFlip && FAILED(hr))
    {
        Logf("[AutoPacer v13] Failed with waitable (0x%08X), retrying without...", (unsigned)hr);
        hr = oCreateSCForHwnd(pFactory, pDevice, hWnd, pDesc,
                               pFullscreenDesc, pRestrictToOutput, ppSwapChain);
        if (SUCCEEDED(hr) && *ppSwapChain)
        {
            Log("[AutoPacer v13] Succeeded without waitable");
            HookPresentOnSwapchain(*ppSwapChain);
        }
    }

    return hr;
}

// ── Hook factory vtable ───────────────────────────────────────────────────────
static void HookFactory(IDXGIFactory* factory)
{
    void** vt = *(void***)factory;

    if (!oCreateSC)
    {
        if (PatchVTable(vt, SLOT_CreateSwapChain, (void*)HookedCreateSwapChain, (void**)&oCreateSC))
            Log("[AutoPacer v13] CreateSwapChain hook installed");
        else
            Log("[AutoPacer v13] ERROR: CreateSwapChain patch failed");
    }

    // Also hook IDXGIFactory2::CreateSwapChainForHwnd if available
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2)))
    {
        void** vt2 = *(void***)factory2;
        if (!oCreateSCForHwnd)
        {
            if (PatchVTable(vt2, SLOT_CreateSwapChainForHwnd,
                (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCForHwnd))
                Log("[AutoPacer v13] CreateSwapChainForHwnd hook installed");
            else
                Log("[AutoPacer v13] ERROR: CreateSwapChainForHwnd patch failed");
        }
        factory2->Release();
    }
}

// ── Init: hook factory via CreateDXGIFactory ─────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);
    Sleep(1500); // let game load, then hook before it creates swapchain

    Log("[AutoPacer v13] Initializing - hooking DXGI factory...");

    // Create a factory to get its vtable
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory)
    {
        Logf("[AutoPacer v13] CreateDXGIFactory1 failed: 0x%08X", (unsigned)hr);
        return 1;
    }

    HookFactory(factory);
    factory->Release();

    Log("[AutoPacer v13] Factory hooks installed. Waiting for game swapchain creation...");
    Log("[AutoPacer v13] If game already created swapchain, Present hook will set up waitable on first frame.");
    Beep(880, 150);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}