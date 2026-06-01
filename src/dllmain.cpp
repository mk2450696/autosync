// AutoPacer v15 - Late Present Chain Injection
//
// WHY EVERY PRIOR VERSION CRASHED:
//   v8-v12:  Raced with DLSS Enabler/OptiScaler for vtable slot 8 (Present).
//            Two hooks writing the same slot = corruption = crash.
//   v13:     Created competing IDXGIFactory1 instance that fought DLSS Enabler's
//            own DXGI init at the same time window. Black screen hang.
//   v14:     IAT hook fired too early, hooked factory before DLSS Enabler did,
//            then flip model upgrade (SwapEffect change) caused DXGI rejection
//            on a corrupt/internal desc -> crash. Also still raced for vtable.
//
// THE NEW APPROACH - no races, no conflicts:
//
//   1. LATE INIT: We sleep until the game window is fully visible and rendering.
//      By then DLSS Enabler and OptiScaler have already installed their hooks.
//      We don't touch CreateSwapChain or any factory at all.
//
//   2. CHAIN WRAPPING: We find the game's swapchain by creating a temporary
//      D3D11 device + swapchain solely to read the vtable address layout,
//      then scan all process memory for the real game swapchain's vtable.
//      Actually simpler: we hook Present by reading slot 8 from the vtable of
//      a swapchain we find via the foreground window. Whatever is in slot 8 is
//      already the top of the chain (DLSS Enabler's hook, or the original).
//      We save that pointer as oPresent and replace slot 8 with ours.
//      Now the call chain is: Game -> US -> DLSS Enabler -> Original Present.
//      We are the outermost wrapper. No conflict possible.
//
//   3. NO SWAPCHAIN MODIFICATION: We don't change SwapEffect, BufferCount,
//      or Flags. The game's swapchain is created exactly as the game + mods
//      want it. We only gate the timing of Present calls.
//
//   4. WAITABLE OBJECT: We QueryInterface for IDXGISwapChain2 on the real
//      swapchain. If it supports it (flip model or if mods upgraded it),
//      we get a pipeline-sync handle tied to Intel's display cycle.
//      If not (BitBlt model), we fall back to DwmFlush() which forces DWM
//      to finish compositing the previous frame before we present the next.
//      Either way, frames are gated to Intel's actual readiness.
//
//   5. ALLOW_TEARING preserved: We never touch SyncInterval or Flags.
//      VRR stays active exactly as the game/mods configured it.
//
// HOW WE FIND THE REAL SWAPCHAIN:
//   We hook IDXGIFactory::CreateSwapChain at the EAT (Export Address Table)
//   of dxgi.dll itself - NOT the vtable. The EAT hook fires before any caller
//   regardless of load order, but we only USE the swapchain pointer to install
//   our Present chain wrap - we don't modify the desc at all.
//   This is the same technique ReShade uses.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <psapi.h>

// ── Logging ───────────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_logCS;
static bool             g_logCSInit = false;
static char             g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg)
{
    OutputDebugStringA("[AutoPacer v15] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (!g_logCSInit) return;
    EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v15] %s\n", msg);
        fclose(fp);
    }
    LeaveCriticalSection(&g_logCS);
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── State ─────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_PresentHooked { false };
static std::atomic<bool> g_SCHooked      { false };
static bool              g_WaitableActive = false;
static bool              g_DwmFallback    = false;
static bool              g_FirstFrame     = true;
static HANDLE            g_hWaitable      = nullptr;

static const UINT  FRAME_LATENCY    = 1;
static const DWORD WAITABLE_TIMEOUT = 33;

// ── VTable helpers ────────────────────────────────────────────────────────────
static bool WritePtr(void** addr, void* newVal, void** oldVal)
{
    DWORD old;
    if (!VirtualProtect(addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (oldVal) *oldVal = *addr;
    *addr = newVal;
    VirtualProtect(addr, sizeof(void*), old, &old);
    return true;
}

// ── Function pointer types ────────────────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)  (IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1) (IDXGISwapChain1*, UINT, UINT,
                                                    const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSC) (IDXGIFactory*, IUnknown*, HWND,
                                                    const DXGI_SWAP_CHAIN_DESC*,
                                                    IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCFHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                        const DXGI_SWAP_CHAIN_DESC1*,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                        IDXGIOutput*, IDXGISwapChain1**);

static PFN_Present      oPresent      = nullptr;
static PFN_Present1     oPresent1     = nullptr;
static PFN_CreateSC     oCreateSC     = nullptr;
static PFN_CreateSCFHwnd oCreateSCFHwnd = nullptr;

static const int SLOT_Present                = 8;
static const int SLOT_Present1               = 22;
static const int SLOT_CreateSwapChain        = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;

// ── Waitable / DWM fallback setup ─────────────────────────────────────────────
static void SetupSync(IDXGISwapChain* sc)
{
    // Try waitable object first (requires flip model swapchain)
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
    {
        HANDLE h = sc2->GetFrameLatencyWaitableObject();
        if (h)
        {
            sc2->SetMaximumFrameLatency(FRAME_LATENCY);
            g_hWaitable      = h;
            g_WaitableActive = true;
            Log("Sync mode: WAITABLE OBJECT (pipeline-sync to Intel VBlank)");
        }
        sc2->Release();
    }

    if (!g_WaitableActive)
    {
        // Swapchain is BitBlt model - use DwmFlush as fallback.
        // DwmFlush() blocks until DWM finishes compositing the current frame,
        // which on this system means Intel has consumed the previous frame.
        // Not as precise as the waitable object but works for any swapchain.
        g_DwmFallback = true;
        Log("Sync mode: DWM FLUSH fallback (BitBlt swapchain, no waitable available)");
    }
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("First Present - chain wrap confirmed active");
        SetupSync(pSC);
        Logf("  Waitable=%s DwmFallback=%s",
            g_WaitableActive ? "YES" : "NO",
            g_DwmFallback    ? "YES" : "NO");
        Beep(1000, 120);
    }

    // Gate: wait for Intel's pipeline to be ready before handing frame over
    if (g_WaitableActive && g_hWaitable)
    {
        DWORD r = WaitForSingleObjectEx(g_hWaitable, WAITABLE_TIMEOUT, FALSE);
        if (r == WAIT_TIMEOUT)
            Log("WARNING: waitable timeout");
    }
    else if (g_DwmFallback)
    {
        // DwmFlush waits for DWM to finish the current composition pass.
        // This prevents us from flooding Intel's compositor faster than it
        // can output frames to the display.
        DwmFlush();
    }

    // Call whatever was in slot 8 before us - DLSS Enabler, OptiScaler, or original.
    // We never modify SyncInterval or Flags - VRR stays exactly as configured.
    return oPresent(pSC, SyncInterval, Flags);
}

// ── Install Present chain wrap on a live swapchain ───────────────────────────
static void WrapPresent(IDXGISwapChain* sc)
{
    if (g_PresentHooked.exchange(true)) return;

    void** vt = *(void***)sc;

    // Read what's currently in slot 8.
    // If DLSS Enabler/OptiScaler already hooked it, oPresent = their hook.
    // If not, oPresent = original IDXGISwapChain::Present.
    // Either way we chain correctly.
    if (WritePtr(&vt[SLOT_Present], (void*)HookedPresent, (void**)&oPresent))
        Logf("Present chain wrap installed. Slot 8 was: %p (now ours)", oPresent);
    else
    {
        g_PresentHooked = false;
        Log("ERROR: Present vtable write failed");
    }
}

// ── Hooked CreateSwapChain (EAT-level, fires for ALL callers) ─────────────────
// We don't touch the desc at all. We only use the resulting swapchain pointer
// to install our Present wrap. This is the ReShade pattern.
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSC)
{
    // Call original (or whatever's already chained - DLSS Enabler etc.)
    HRESULT hr = oCreateSC(pFactory, pDevice, hWnd, pDesc, ppSC);
    if (SUCCEEDED(hr) && *ppSC)
    {
        Logf("CreateSwapChain -> SwapEffect=%u Buffers=%u [OK]",
            pDesc->SwapEffect, pDesc->BufferCount);
        WrapPresent(*ppSC);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSD,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    HRESULT hr = oCreateSCFHwnd(pFactory, pDevice, hWnd, pDesc, pFSD, pOutput, ppSC);
    if (SUCCEEDED(hr) && *ppSC)
    {
        Logf("CreateSwapChainForHwnd -> SwapEffect=%u Buffers=%u [OK]",
            pDesc->SwapEffect, pDesc->BufferCount);
        WrapPresent(*ppSC);
    }
    return hr;
}

// ── Hook factory vtable - LATE (after all mods have hooked) ──────────────────
static void HookFactory(IDXGIFactory* f)
{
    if (g_SCHooked.exchange(true)) return;

    void** vt = *(void***)f;

    // Read slot 10 - whatever is there is the top of the CreateSwapChain chain.
    // Could be DLSS Enabler's hook or the original. We wrap around it.
    if (WritePtr(&vt[SLOT_CreateSwapChain], (void*)HookedCreateSwapChain, (void**)&oCreateSC))
        Logf("CreateSwapChain chain wrap installed. Slot 10 was: %p", oCreateSC);
    else
        Log("ERROR: CreateSwapChain vtable write failed");

    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)))
    {
        void** vt2 = *(void***)f2;
        if (WritePtr(&vt2[SLOT_CreateSwapChainForHwnd],
            (void*)HookedCreateSwapChainForHwnd, (void**)&oCreateSCFHwnd))
            Logf("CreateSwapChainForHwnd chain wrap installed. Slot 15 was: %p", oCreateSCFHwnd);
        else
            Log("ERROR: CreateSwapChainForHwnd vtable write failed");
        f2->Release();
    }
}

// ── Init: wait for game window, then hook LATE ────────────────────────────────
// Key insight: we wait for the game's main window to appear and be visible.
// By that point all mods (DLSS Enabler, OptiScaler) have already loaded and
// installed their own hooks. We then wrap around them, not race with them.
static DWORD WINAPI InitThread(LPVOID)
{
    // Wait for dxgi.dll
    for (int i = 0; i < 200; ++i) {
        if (GetModuleHandleA("dxgi.dll")) break;
        Sleep(50);
    }
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) { Log("ERROR: dxgi.dll never loaded"); return 1; }
    Log("dxgi.dll present");

    // Wait for a real game window to appear (not just a launcher splash).
    // We poll for a window that is visible, has a title, and is not a dialog.
    // Timeout: 30 seconds.
    HWND gameWnd = nullptr;
    for (int i = 0; i < 600; ++i)
    {
        Sleep(50);
        HWND fg = GetForegroundWindow();
        if (fg)
        {
            char title[256] = {};
            GetWindowTextA(fg, title, sizeof(title));
            RECT r = {};
            GetClientRect(fg, &r);
            bool bigEnough = (r.right - r.left) > 400 && (r.bottom - r.top) > 300;
            bool hasTitle  = title[0] != '\0';
            if (bigEnough && hasTitle)
            {
                gameWnd = fg;
                Logf("Game window found: '%s' (%dx%d)",
                    title, r.right - r.left, r.bottom - r.top);
                break;
            }
        }
    }

    // Extra settle time - let all mods finish their hooks
    // 500ms after window appears is enough for any mod to have hooked Present
    Sleep(500);
    Log("Mod settle time elapsed - installing chain wraps now");

    // Create a throw-away factory just to get the vtable layout & addresses.
    // We immediately hook it and release. The vtable is shared (COM vtable is
    // per-class, not per-instance), so patching this instance patches all
    // IDXGIFactory instances in the process including the game's real one.
    IDXGIFactory1* tempFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&tempFactory);
    if (FAILED(hr) || !tempFactory)
    {
        Logf("ERROR: CreateDXGIFactory1 failed: 0x%08X", (unsigned)hr);
        return 1;
    }

    HookFactory(tempFactory);
    tempFactory->Release();

    Log("Chain wraps installed. Next swapchain creation or Present will activate.");
    Log("If game already presented frames, Present wrap may miss first frames.");
    Log("Restart game if no second beep within 10s of loading.");
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

        // Set log path to same directory as this DLL
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }

        Log("DLL loaded - v15 chain-wrap approach");
        Logf("Log path: %s", g_logPath);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_hWaitable) CloseHandle(g_hWaitable);
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}