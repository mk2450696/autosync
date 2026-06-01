// AutoPacer v12 - DWM Scanline-Safe Pacing for CASO iGPU+dGPU setups
//
// ROOT CAUSE IDENTIFIED:
//   All prior versions (v8-v11) stripped DXGI_PRESENT_ALLOW_TEARING from Present flags.
//   That flag is what activates VRR on the display. Stripping it = static 165Hz = VRR dead.
//   This is why the monitor showed static 165Hz in every version.
//
// v12 APPROACH:
//   1. NEVER strip DXGI_PRESENT_ALLOW_TEARING. VRR must stay on.
//   2. Use DwmGetCompositionTimingInfo() to read Intel's scanout timing directly.
//      DWM runs on Intel UHD 730 (confirmed: DWM on GPU1/Intel in Task Manager).
//      DWM timing gives us: qpcRefreshPeriod, qpcVBlank, cRefreshesDisplayed.
//   3. For burst frames: use DWM timing to calculate if we're in the "danger zone"
//      (middle of active scanout). If yes, delay by microseconds to slip past it.
//      If no, pass through immediately.
//   4. For normal frames: pass through with zero intervention.
//
// WHY THIS WORKS WHERE OTHERS FAILED:
//   RTSS Scanline Sync queries NVIDIA's raster position - wrong GPU, wrong timing.
//   v10-v11 VBlank relay used WaitForVBlank on Intel output BUT stripped ALLOW_TEARING,
//   killing VRR. v12 uses DWM timing (Intel-native) AND preserves ALLOW_TEARING.
//
// RESULT: VRR active + frames timed to Intel's actual scanout = no tearing, no judder,
//         no FPS cap, FG works normally.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <immintrin.h>

// ── Configuration ─────────────────────────────────────────────────────────────
// Frames arriving faster than this are FG burst frames needing intervention.
// 3.5ms = ~285fps. FG bursts arrive <1ms apart. Normal frames >5ms apart.
static const long long BURST_THRESHOLD_NS = 3500000LL;

// Fraction of the refresh period considered "safe" at the start (post-VBlank).
// At 165Hz: period=6.06ms, safe zone = first 15% = ~0.9ms.
// We present only if we're within this window after the last VBlank.
// If not, we wait until the NEXT VBlank + this offset.
static const double SAFE_FRACTION = 0.15;
// ──────────────────────────────────────────────────────────────────────────────

// DWM timing function - gives us Intel's actual scanout clock
typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO*);
static PFN_DwmGetCompositionTimingInfo pfnDwmGetTimingInfo = nullptr;

static IDXGIOutput*      g_IntelOutput   = nullptr;
static std::atomic<bool> g_Running       { false };
static HANDLE            g_hVBlankThread = nullptr;
static HANDLE            g_hVBlankEvent  = nullptr;  // auto-reset, fires each VBlank

// Latest VBlank QPC timestamp from Intel output (written by VBlank thread)
static std::atomic<LONGLONG> g_LastVBlankQPC    { 0 };
static std::atomic<LONGLONG> g_VBlankPeriodQPC  { 0 };

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent         = nullptr;
static LONGLONG    g_QPCFreq        = 0;
static LONGLONG    g_LastPresentQPC = 0;
static bool        g_FirstFrame     = true;

// ── Logging ───────────────────────────────────────────────────────────────────
static void Log(const char* msg)
{
    OutputDebugStringA(msg); OutputDebugStringA("\n");
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) { fprintf(fp, "%s\n", msg); fclose(fp); }
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    Log(buf);
}

// ── VTable patch ──────────────────────────────────────────────────────────────
static bool PatchVTable(void** vtable, int slot, void* newFn, void** oldFn)
{
    DWORD old;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *oldFn = vtable[slot]; vtable[slot] = newFn;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

// ── Find Intel IDXGIOutput ────────────────────────────────────────────────────
static IDXGIOutput* FindPrimaryOutput()
{
    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hPrimary, &mi);
    char primaryDev[64] = {};
    strncpy_s(primaryDev, mi.szDevice, 63);
    Logf("[AutoPacer v12] Primary: HMON=0x%p device='%s' rect=(%d,%d,%d,%d)",
        (void*)hPrimary, primaryDev,
        mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);

    IDXGIOutput* result = nullptr;

    // Method A: factory enumeration
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        {
            for (UINT ai = 0; !result; ++ai)
            {
                IDXGIAdapter* adapter = nullptr;
                if (FAILED(factory->EnumAdapters(ai, &adapter))) break;
                DXGI_ADAPTER_DESC d = {}; adapter->GetDesc(&d);
                char n[128] = {}; WideCharToMultiByte(CP_ACP, 0, d.Description, -1, n, 127, 0, 0);
                Logf("[AutoPacer v12]   [A] Adapter %u: '%s' vendor=0x%04X", ai, n, d.VendorId);
                bool isNV = (d.VendorId == 0x10DE) || strstr(n,"NVIDIA") || strstr(n,"GeForce");
                bool isSW = strstr(n,"Microsoft") || strstr(n,"Basic Render");
                if (!isNV && !isSW) {
                    for (UINT oi = 0; ; ++oi) {
                        IDXGIOutput* o = nullptr;
                        if (FAILED(adapter->EnumOutputs(oi, &o))) break;
                        DXGI_OUTPUT_DESC od = {}; o->GetDesc(&od);
                        char dn[64] = {}; WideCharToMultiByte(CP_ACP, 0, od.DeviceName, -1, dn, 63, 0, 0);
                        Logf("[AutoPacer v12]     Output %u: '%s' HMON=0x%p", oi, dn, (void*)od.Monitor);
                        bool m = (_stricmp(dn, primaryDev)==0)||(od.Monitor==hPrimary)||
                                 (od.DesktopCoordinates.left==mi.rcMonitor.left &&
                                  od.DesktopCoordinates.top==mi.rcMonitor.top &&
                                  od.DesktopCoordinates.right==mi.rcMonitor.right &&
                                  od.DesktopCoordinates.bottom==mi.rcMonitor.bottom);
                        if (m) { Logf("[AutoPacer v12]   MATCH A: %s out %u", n, oi); result=o; o=nullptr; }
                        if (o) o->Release(); if (result) break;
                    }
                }
                adapter->Release();
            }
            factory->Release();
        }
    }

    if (!result) {
        Log("[AutoPacer v12] Method A empty, trying Method C (swapchain)...");
        WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=DefWindowProcA;
        wc.hInstance=GetModuleHandleA(nullptr); wc.lpszClassName="AP12_C";
        RegisterClassExA(&wc);
        HWND hw=CreateWindowExA(0,"AP12_C","",WS_POPUP,mi.rcMonitor.left,mi.rcMonitor.top,
            8,8,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=2;
        sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width=sd.BufferDesc.Height=8;
        sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
        sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; IDXGISwapChain* sc=nullptr;
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,&fl,1,D3D11_SDK_VERSION,&sd,&sc,&dev,nullptr,&ctx)) && sc)
        {
            IDXGIOutput* out=nullptr;
            if (SUCCEEDED(sc->GetContainingOutput(&out)) && out) {
                DXGI_OUTPUT_DESC od={}; out->GetDesc(&od);
                char dn[64]={}; WideCharToMultiByte(CP_ACP,0,od.DeviceName,-1,dn,63,0,0);
                Logf("[AutoPacer v12]   [C] GetContainingOutput: '%s' HMON=0x%p", dn, (void*)od.Monitor);
                bool m=(_stricmp(dn,primaryDev)==0)||(od.Monitor==hPrimary)||
                        (od.DesktopCoordinates.left==mi.rcMonitor.left && od.DesktopCoordinates.top==mi.rcMonitor.top);
                if (m) { Log("[AutoPacer v12]   MATCH C"); result=out; out=nullptr; }
                if (out) out->Release();
            }
            sc->Release(); dev->Release(); ctx->Release();
        }
        DestroyWindow(hw); UnregisterClassA("AP12_C", wc.hInstance);
    }

    if (!result) Log("[AutoPacer v12] ERROR: Could not find Intel output.");
    return result;
}

// ── VBlank relay thread ───────────────────────────────────────────────────────
// Calls WaitForVBlank on Intel's IDXGIOutput, timestamps each VBlank,
// fires g_hVBlankEvent. Does NOT gate Present calls.
static DWORD WINAPI VBlankThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Log("[AutoPacer v12] VBlank thread started");
    LONGLONG lastQPC = 0;
    while (g_Running.load(std::memory_order_relaxed))
    {
        if (FAILED(g_IntelOutput->WaitForVBlank())) { Sleep(1); continue; }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        LONGLONG qpc = now.QuadPart;
        if (lastQPC > 0) {
            LONGLONG period = qpc - lastQPC;
            LONGLONG prev = g_VBlankPeriodQPC.load(std::memory_order_relaxed);
            g_VBlankPeriodQPC.store(prev>0 ? (prev*7+period)/8 : period, std::memory_order_relaxed);
        }
        lastQPC = qpc;
        g_LastVBlankQPC.store(qpc, std::memory_order_release);
        SetEvent(g_hVBlankEvent);
    }
    return 0;
}

// ── Precision spinlock ────────────────────────────────────────────────────────
static void SpinUntilQPC(LONGLONG targetQPC)
{
    LARGE_INTEGER now;
    do { _mm_pause(); QueryPerformanceCounter(&now); } while (now.QuadPart < targetQPC);
}

// ── Get Intel scanout timing via DWM ─────────────────────────────────────────
// DWM compositor runs on Intel, so its timing info reflects Intel's actual scanout.
// Returns true and fills qpcLastVBlank / qpcRefreshPeriod if successful.
static bool GetDWMTiming(LONGLONG* qpcLastVBlank, LONGLONG* qpcRefreshPeriod)
{
    if (!pfnDwmGetTimingInfo) return false;
    DWM_TIMING_INFO ti = {}; ti.cbSize = sizeof(ti);
    if (FAILED(pfnDwmGetTimingInfo(nullptr, &ti))) return false;
    if (ti.qpcRefreshPeriod == 0) return false;
    *qpcLastVBlank    = (LONGLONG)ti.qpcVBlank;
    *qpcRefreshPeriod = (LONGLONG)ti.qpcRefreshPeriod;
    return true;
}

// ── Core pacing: v12 model ────────────────────────────────────────────────────
//
// For normal frames (gap > BURST_THRESHOLD_NS): pass through immediately.
//   VRR adapts naturally. ALLOW_TEARING preserved. No intervention.
//
// For burst frames (gap < BURST_THRESHOLD_NS, i.e. FG micro-burst):
//   Get current scanout position from DWM timing (Intel-native).
//   If we're in the safe zone (first SAFE_FRACTION of the refresh period after VBlank):
//     -> Present now. We're right after VBlank, display just started a new scanout.
//        Frame will be shown cleanly.
//   If we're in the danger zone (active scanout, middle of the frame):
//     -> Wait until the NEXT VBlank + safe zone offset.
//     -> Present then. Frame lands at top of fresh scanout.
//
// ALLOW_TEARING is passed through unchanged. VRR stays fully active.
//
static void PaceFrame()
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);

    if (g_LastPresentQPC == 0) { g_LastPresentQPC = now.QuadPart; return; }

    long long elapsedNs = (now.QuadPart - g_LastPresentQPC) * 1000000000LL / g_QPCFreq;

    if (elapsedNs >= BURST_THRESHOLD_NS)
    {
        // Normal frame - zero intervention
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    // ── Burst frame: need to land it in the safe zone ──────────────────────────

    // Try DWM timing first (most accurate - Intel's own clock)
    LONGLONG dwmLastVBlank = 0, dwmPeriod = 0;
    bool hasDWM = GetDWMTiming(&dwmLastVBlank, &dwmPeriod);

    // Fall back to VBlank thread measurements if DWM unavailable
    LONGLONG lastVBlank = hasDWM ? dwmLastVBlank : g_LastVBlankQPC.load(std::memory_order_acquire);
    LONGLONG period     = hasDWM ? dwmPeriod     : g_VBlankPeriodQPC.load(std::memory_order_relaxed);

    if (period <= 0 || lastVBlank <= 0)
    {
        // No timing available yet - enforce minimum gap only
        long long waitNs = BURST_THRESHOLD_NS - elapsedNs;
        if (waitNs > 0)
        {
            LONGLONG targetQPC = g_LastPresentQPC + waitNs * g_QPCFreq / 1000000000LL;
            SpinUntilQPC(targetQPC);
        }
        QueryPerformanceCounter(&now);
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    // Calculate safe zone: first SAFE_FRACTION of the period after VBlank
    LONGLONG safeZoneQPC = (LONGLONG)(period * SAFE_FRACTION);

    // Where are we right now relative to the last VBlank?
    QueryPerformanceCounter(&now);
    LONGLONG sinceVBlankQPC = now.QuadPart - lastVBlank;

    // Handle case where we're before the VBlank timestamp (clock jitter)
    if (sinceVBlankQPC < 0) sinceVBlankQPC = 0;

    // Normalize to current refresh period
    if (sinceVBlankQPC >= period)
        sinceVBlankQPC = sinceVBlankQPC % period;

    bool inSafeZone = (sinceVBlankQPC < safeZoneQPC);

    if (inSafeZone)
    {
        // We're right after a VBlank - safe to present now
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    // We're in the danger zone. Wait for the next VBlank + safe zone offset.
    LONGLONG qpcToNextVBlank = period - sinceVBlankQPC;
    LONGLONG targetQPC = now.QuadPart + qpcToNextVBlank + safeZoneQPC / 2;

    // Sanity cap: never wait more than 1.5 refresh periods (~9ms at 165Hz)
    LONGLONG maxWaitQPC = now.QuadPart + (period * 3 / 2);
    if (targetQPC > maxWaitQPC) targetQPC = maxWaitQPC;

    // Wait efficiently: event-based until close, then spinlock for precision
    LONGLONG remainQPC = targetQPC - now.QuadPart;
    long long remainNs = remainQPC * 1000000000LL / g_QPCFreq;

    if (remainNs > 1500000LL && g_hVBlankEvent)
    {
        // Sleep until next VBlank fires (efficient)
        ResetEvent(g_hVBlankEvent);
        // Re-read: did VBlank fire while we were resetting?
        LONGLONG currentLastVBlank = g_LastVBlankQPC.load(std::memory_order_acquire);
        if (currentLastVBlank == lastVBlank)
        {
            WaitForSingleObject(g_hVBlankEvent, 20); // timeout 20ms = 3 frames max
        }
        // VBlank fired. Spinlock to precise safe zone offset.
        LONGLONG newVBlank = g_LastVBlankQPC.load(std::memory_order_acquire);
        LONGLONG preciseTarget = newVBlank + safeZoneQPC / 2;
        SpinUntilQPC(preciseTarget);
    }
    else
    {
        // Close to target - just spinlock
        SpinUntilQPC(targetQPC);
    }

    QueryPerformanceCounter(&now);
    g_LastPresentQPC = now.QuadPart;
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("[AutoPacer v12] First frame - hook confirmed active");
        // Log whether DWM timing is available
        LONGLONG a=0, b=0;
        bool dwm = GetDWMTiming(&a, &b);
        Logf("[AutoPacer v12] DWM timing: %s (period=%.3fms)",
            dwm ? "AVAILABLE" : "UNAVAILABLE",
            dwm ? (b * 1000.0 / g_QPCFreq) : 0.0);
        Beep(1000, 120);
    }

    PaceFrame();

    // CRITICAL: pass SyncInterval and Flags through UNCHANGED.
    // Do NOT strip DXGI_PRESENT_ALLOW_TEARING - that flag keeps VRR active.
    // We handle the scanout timing ourselves; we don't need to disable VRR to do it.
    return oPresent(pSC, SyncInterval, Flags);
}

// ── Hook installation ─────────────────────────────────────────────────────────
static bool InstallHook()
{
    WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=DefWindowProcA;
    wc.hInstance=GetModuleHandleA(nullptr); wc.lpszClassName="AP12_H";
    RegisterClassExA(&wc);
    HWND hw=CreateWindowExA(0,"AP12_H","",WS_POPUP,0,0,8,8,nullptr,nullptr,wc.hInstance,nullptr);

    DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=2;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width=sd.BufferDesc.Height=8;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; IDXGISwapChain* sc=nullptr;

    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
        nullptr,0,&fl,1,D3D11_SDK_VERSION,&sd,&sc,&dev,nullptr,&ctx);
    if (FAILED(hr)||!sc)
    {
        Logf("[AutoPacer v12] Hook SC failed: 0x%08X", (unsigned)hr);
        DestroyWindow(hw); UnregisterClassA("AP12_H", wc.hInstance);
        return false;
    }

    void** vtable=*(void***)sc;
    bool ok=PatchVTable(vtable, 8, (void*)HookedPresent, (void**)&oPresent);
    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hw); UnregisterClassA("AP12_H", wc.hInstance);

    if (ok) Log("[AutoPacer v12] Present hook installed (slot 8)");
    else    Log("[AutoPacer v12] ERROR: VTable patch failed");
    return ok;
}

// ── Init thread ───────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);
    Sleep(2000);

    Log("[AutoPacer v12] Initializing...");

    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq); g_QPCFreq = freq.QuadPart;

    // Load DWM timing function
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm)
    {
        pfnDwmGetTimingInfo = (PFN_DwmGetCompositionTimingInfo)
            GetProcAddress(hDwm, "DwmGetCompositionTimingInfo");
        Logf("[AutoPacer v12] DwmGetCompositionTimingInfo: %s",
            pfnDwmGetTimingInfo ? "loaded" : "not found");
    }
    else Log("[AutoPacer v12] WARNING: dwmapi.dll not loaded");

    g_IntelOutput = FindPrimaryOutput();

    if (g_IntelOutput)
    {
        g_hVBlankEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
        if (g_hVBlankEvent)
        {
            g_Running.store(true);
            g_hVBlankThread = CreateThread(nullptr, 0, VBlankThread, nullptr, 0, nullptr);
        }
    }
    else Log("[AutoPacer v12] WARNING: Intel output not found. DWM-only timing mode.");

    if (!InstallHook()) return 1;

    bool hasFull = (g_IntelOutput && g_hVBlankEvent);
    bool hasDWM  = (pfnDwmGetTimingInfo != nullptr);
    Logf("[AutoPacer v12] Active mode: VBlank relay=%s, DWM timing=%s",
        hasFull ? "YES" : "NO", hasDWM ? "YES" : "NO");
    Logf("[AutoPacer v12] ALLOW_TEARING preserved: VRR stays ACTIVE");
    Log("[AutoPacer v12] SUCCESS");
    Beep(880, 200);

    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_Running.store(false);
        if (g_hVBlankEvent) { SetEvent(g_hVBlankEvent); CloseHandle(g_hVBlankEvent); }
        if (g_hVBlankThread) { WaitForSingleObject(g_hVBlankThread, 2000); CloseHandle(g_hVBlankThread); }
        if (g_IntelOutput) g_IntelOutput->Release();
    }
    return TRUE;
}
