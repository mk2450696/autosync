// AutoPacer v11 - Scanline-Safe Frame Pacing for CASO iGPU+dGPU setups
//
// Setup: RTX 3060 Ti renders. Intel UHD 730 drives display via CASO Tier 3.
// Problem: FG (DLSS/FSR) bursts 4 frames in ~2ms. Intel display scans at 165Hz
//          (6.06ms per frame). Burst frames arrive mid-scanout -> tearing.
//          Prior VBlank serialization (v10) fixed tearing but serialized ALL
//          frames through VBlank slots -> hard 165fps cap + FG timing broken.
//
// v11 Fix: Never block normal frames. Only delay burst frames (inter-frame gap
//          < SAFE_GAP_NS). When a burst is detected, calculate when the NEXT
//          safe presentation window opens (top of scanout = start of VBlank),
//          wait only until then, then present with SyncInterval=0.
//
// Key insight: A frame presented right AFTER VBlank starts is safe because
// the display just finished its previous scanout. We use WaitForVBlank() on a
// background thread purely as a timing reference - it fires every 6.06ms.
// Burst frames are delayed to the next VBlank edge. Non-burst frames pass through
// immediately. This gives us: tear-free burst handling + zero added latency on
// normal frames + VRR working across the full 48-165Hz range.
//
// FG artifact fix: We no longer hold FG frames hostage in a semaphore queue.
// Each frame is delayed AT MOST to the next VBlank edge (~6ms in worst case),
// and only if it arrived <SAFE_GAP_NS after the previous one.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <immintrin.h>

// ── Configuration ─────────────────────────────────────────────────────────────
// Frames arriving faster than this are considered burst frames from FG.
// 3.5ms = 285fps equivalent. Real rendered frames rarely arrive faster than this.
// FG bursts typically arrive <1ms apart. This gives safe margin.
static const long long SAFE_GAP_NS = 3500000LL;

// How long after VBlank start the scanout is still in the "safe zone"
// (display hasn't started drawing yet). ~0.5ms at 165Hz.
static const long long SAFE_ZONE_NS = 500000LL;

// Timeout waiting for a VBlank edge in burst case. 50ms = 2 full frames max.
static const DWORD BURST_WAIT_MS = 50;
// ──────────────────────────────────────────────────────────────────────────────

static IDXGIOutput*      g_IntelOutput   = nullptr;
static std::atomic<bool> g_Running       { false };
static HANDLE            g_hRelayThread  = nullptr;

// VBlank timing: updated by relay thread every VBlank.
// Written by one thread, read by Present thread. QPC timestamp.
static std::atomic<LONGLONG> g_LastVBlankQPC { 0 };
static std::atomic<LONGLONG> g_VBlankPeriodQPC { 0 }; // measured period between VBlanks

// Signaled on each VBlank for burst frames to wake on.
static HANDLE g_hVBlankEvent = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent      = nullptr;
static LONGLONG    g_QPCFreq     = 0;
static LONGLONG    g_LastPresentQPC = 0;
static bool        g_FirstFrame  = true;

// ── Logging ───────────────────────────────────────────────────────────────────
static void Log(const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
    }
}
static void Logf(const char* fmt, ...)
{
    char buf[512];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

// ── VTable patch ──────────────────────────────────────────────────────────────
static bool PatchVTable(void** vtable, int slot, void* newFn, void** oldFn)
{
    DWORD oldProt;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    *oldFn = vtable[slot];
    vtable[slot] = newFn;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProt, &oldProt);
    return true;
}

// ── Find Intel IDXGIOutput (same 3-method approach from v10, known working) ───
static IDXGIOutput* FindPrimaryOutput()
{
    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hPrimary, &mi);

    char primaryDev[64] = {};
    strncpy_s(primaryDev, mi.szDevice, 63);

    Logf("[AutoPacer v11] Primary monitor: HMON=0x%p device='%s' rect=(%d,%d,%d,%d)",
        (void*)hPrimary, primaryDev,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right, mi.rcMonitor.bottom);

    IDXGIOutput* result = nullptr;

    // ── Method A: IDXGIFactory1 enumeration ──────────────────────────────────
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        {
            for (UINT ai = 0; !result; ++ai)
            {
                IDXGIAdapter* adapter = nullptr;
                if (FAILED(factory->EnumAdapters(ai, &adapter))) break;

                DXGI_ADAPTER_DESC adesc = {};
                adapter->GetDesc(&adesc);
                char aname[128] = {};
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, aname, 127, nullptr, nullptr);
                Logf("[AutoPacer v11]   [A] Adapter %u: '%s' vendor=0x%04X", ai, aname, adesc.VendorId);

                bool isNV = (adesc.VendorId == 0x10DE) || strstr(aname, "NVIDIA") || strstr(aname, "GeForce");
                bool isSW = strstr(aname, "Microsoft") || strstr(aname, "Basic Render");

                if (!isNV && !isSW)
                {
                    for (UINT oi = 0; ; ++oi)
                    {
                        IDXGIOutput* output = nullptr;
                        if (FAILED(adapter->EnumOutputs(oi, &output))) break;

                        DXGI_OUTPUT_DESC odesc = {};
                        output->GetDesc(&odesc);
                        char devname[64] = {};
                        WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devname, 63, nullptr, nullptr);
                        Logf("[AutoPacer v11]     Output %u: '%s' HMON=0x%p", oi, devname, (void*)odesc.Monitor);

                        bool match = (_stricmp(devname, primaryDev) == 0) ||
                                     (odesc.Monitor == hPrimary) ||
                                     (odesc.DesktopCoordinates.left  == mi.rcMonitor.left  &&
                                      odesc.DesktopCoordinates.top   == mi.rcMonitor.top   &&
                                      odesc.DesktopCoordinates.right  == mi.rcMonitor.right &&
                                      odesc.DesktopCoordinates.bottom == mi.rcMonitor.bottom);
                        if (match) {
                            Logf("[AutoPacer v11]   MATCH Method A: %s output %u", aname, oi);
                            result = output; output = nullptr;
                        }
                        if (output) output->Release();
                        if (result) break;
                    }
                }
                adapter->Release();
            }
            factory->Release();
        }
    }

    if (result) return result;
    Log("[AutoPacer v11] Method A found nothing, trying Method B...");

    // ── Method B: IDXGIFactory6 minimum power preference ─────────────────────
    {
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&f6)))
        {
            for (UINT ai = 0; !result; ++ai)
            {
                IDXGIAdapter1* adapter = nullptr;
                if (FAILED(f6->EnumAdapterByGpuPreference(ai,
                    DXGI_GPU_PREFERENCE_MINIMUM_POWER, __uuidof(IDXGIAdapter1), (void**)&adapter))) break;

                DXGI_ADAPTER_DESC adesc = {};
                adapter->GetDesc(&adesc);
                char aname[128] = {};
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, aname, 127, nullptr, nullptr);

                bool isNV = (adesc.VendorId == 0x10DE) || strstr(aname, "NVIDIA");
                bool isSW = strstr(aname, "Microsoft") || strstr(aname, "Basic Render");

                if (!isNV && !isSW)
                {
                    for (UINT oi = 0; ; ++oi)
                    {
                        IDXGIOutput* output = nullptr;
                        if (FAILED(adapter->EnumOutputs(oi, &output))) break;

                        DXGI_OUTPUT_DESC odesc = {};
                        output->GetDesc(&odesc);
                        char devname[64] = {};
                        WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devname, 63, nullptr, nullptr);

                        bool match = (_stricmp(devname, primaryDev) == 0) || (odesc.Monitor == hPrimary);
                        if (match) {
                            Logf("[AutoPacer v11]   MATCH Method B: %s output %u", aname, oi);
                            result = output; output = nullptr;
                        }
                        if (output) output->Release();
                        if (result) break;
                    }
                }
                adapter->Release();
            }
            f6->Release();
        }
    }

    if (result) return result;
    Log("[AutoPacer v11] Method B found nothing, trying Method C (swapchain)...");

    // ── Method C: temp swapchain GetContainingOutput ──────────────────────────
    {
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "AP11_C";
        RegisterClassExA(&wc);
        HWND hw = CreateWindowExA(0, "AP11_C", "", WS_POPUP, 0, 0, 8, 8,
            nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowPos(hw, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, 8, 8,
            SWP_NOZORDER | SWP_NOACTIVATE);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hw; sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;

        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)) && sc)
        {
            IDXGIOutput* out = nullptr;
            if (SUCCEEDED(sc->GetContainingOutput(&out)) && out)
            {
                DXGI_OUTPUT_DESC odesc = {};
                out->GetDesc(&odesc);
                char devname[64] = {};
                WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devname, 63, nullptr, nullptr);
                Logf("[AutoPacer v11]   [C] GetContainingOutput: '%s' HMON=0x%p", devname, (void*)odesc.Monitor);

                bool match = (_stricmp(devname, primaryDev) == 0) ||
                             (odesc.Monitor == hPrimary) ||
                             (odesc.DesktopCoordinates.left == mi.rcMonitor.left &&
                              odesc.DesktopCoordinates.top  == mi.rcMonitor.top);
                if (match) {
                    Log("[AutoPacer v11]   MATCH Method C: GetContainingOutput");
                    result = out; out = nullptr;
                }
                if (out) out->Release();
            }
            sc->Release(); dev->Release(); ctx->Release();
        }
        DestroyWindow(hw);
        UnregisterClassA("AP11_C", wc.hInstance);
    }

    if (!result)
        Log("[AutoPacer v11] ERROR: All 3 methods failed. Burst-catcher-only mode.");

    return result;
}

// ── VBlank relay thread ───────────────────────────────────────────────────────
// Sole purpose: accurately timestamp each VBlank and signal g_hVBlankEvent.
// Does NOT gate Present calls. Just provides timing reference.
static DWORD WINAPI VBlankRelayThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Log("[AutoPacer v11] VBlank relay thread started");

    LONGLONG lastVBlankQPC = 0;

    while (g_Running.load(std::memory_order_relaxed))
    {
        if (FAILED(g_IntelOutput->WaitForVBlank()))
        {
            Sleep(1);
            continue;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG qpc = now.QuadPart;

        // Measure period between consecutive VBlanks
        if (lastVBlankQPC > 0)
        {
            LONGLONG period = qpc - lastVBlankQPC;
            // Exponential moving average to smooth measurement
            LONGLONG prevPeriod = g_VBlankPeriodQPC.load(std::memory_order_relaxed);
            LONGLONG newPeriod = prevPeriod > 0
                ? (prevPeriod * 7 + period) / 8   // 87.5% old, 12.5% new
                : period;
            g_VBlankPeriodQPC.store(newPeriod, std::memory_order_relaxed);
        }
        lastVBlankQPC = qpc;
        g_LastVBlankQPC.store(qpc, std::memory_order_release);

        // Signal any burst frames waiting for the next VBlank edge
        SetEvent(g_hVBlankEvent);
    }

    Log("[AutoPacer v11] VBlank relay thread exiting");
    return 0;
}

// ── Precision sleep ───────────────────────────────────────────────────────────
static void SleepPreciseNs(long long ns)
{
    if (ns <= 0) return;

    // Use high-res waitable timer for the bulk (leave last 1ms for spinlock)
    if (ns > 1500000LL)
    {
        HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
            0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (hTimer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -((ns - 1000000LL) / 100LL);
            SetWaitableTimerEx(hTimer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(hTimer, (DWORD)(ns / 1000000LL + 10));
            CloseHandle(hTimer);
        }
    }

    // Spinlock for final precision
    LARGE_INTEGER target, now;
    QueryPerformanceCounter(&target);
    target.QuadPart += ns * g_QPCFreq / 1000000000LL;
    do {
        _mm_pause();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target.QuadPart);
}

// ── Core pacing logic ─────────────────────────────────────────────────────────
//
// v11 model:
//   Normal frames (gap > SAFE_GAP_NS): pass through immediately, zero latency.
//   Burst frames  (gap < SAFE_GAP_NS): wait until the next VBlank edge + SAFE_ZONE_NS.
//                                      This lands them right after scanout restarts.
//
// Why this works without the v10 cap problem:
//   - Normal frames are never delayed. VRR adapts to whatever natural rate they arrive.
//   - Burst frames are delayed to the NEXT VBlank, not the current one. One VBlank
//     max wait per burst frame. With 4x FG, bursts are 4 frames per real frame.
//     Each gets its own VBlank slot. 165Hz / 4 = 41fps base game for full 165fps output.
//     At lower base FPS, fewer FG frames per real frame = each gets more time.
//   - The display shows tearing only when a swap happens DURING scanout.
//     By waiting until just after VBlank, we land in the safe zone every time.
//
static void PaceFrame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_LastPresentQPC == 0)
    {
        // First frame - no pacing needed
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    long long elapsedNs = (now.QuadPart - g_LastPresentQPC) * 1000000000LL / g_QPCFreq;

    if (elapsedNs >= SAFE_GAP_NS)
    {
        // Normal frame - pass through immediately
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    // Burst frame detected. Wait for next VBlank edge then present in safe zone.
    LONGLONG vblankPeriodQPC = g_VBlankPeriodQPC.load(std::memory_order_relaxed);
    LONGLONG lastVBlankQPC   = g_LastVBlankQPC.load(std::memory_order_acquire);

    if (vblankPeriodQPC <= 0 || lastVBlankQPC <= 0 || !g_hVBlankEvent)
    {
        // VBlank timing not yet available - fall back to simple gap enforcement
        long long waitNs = SAFE_GAP_NS - elapsedNs;
        SleepPreciseNs(waitNs);
        QueryPerformanceCounter(&now);
        g_LastPresentQPC = now.QuadPart;
        return;
    }

    // Calculate when the next VBlank will fire
    QueryPerformanceCounter(&now);
    LONGLONG qpcSinceLastVBlank = now.QuadPart - lastVBlankQPC;
    LONGLONG qpcToNextVBlank    = vblankPeriodQPC - qpcSinceLastVBlank;

    // If we're already past the next VBlank point, it will fire very soon
    if (qpcToNextVBlank < 0)
        qpcToNextVBlank = vblankPeriodQPC - ((-qpcToNextVBlank) % vblankPeriodQPC);

    long long nsToNextVBlank = qpcToNextVBlank * 1000000000LL / g_QPCFreq;

    // Add SAFE_ZONE_NS to land just inside the safe window after VBlank
    long long waitNs = nsToNextVBlank + SAFE_ZONE_NS;

    // Sanity: if wait would be more than 8ms (more than 1.3 frames at 165Hz),
    // something is off - cap it to avoid excessive lag
    if (waitNs > 8000000LL)
        waitNs = 8000000LL;
    // If negative or tiny, wait at minimum for the safe gap
    if (waitNs < (SAFE_GAP_NS - elapsedNs))
        waitNs = SAFE_GAP_NS - elapsedNs;

    // Wait: use the VBlank event for efficient sleep, then fine-tune with spinlock
    if (nsToNextVBlank > 1000000LL)
    {
        // Reset the event and wait for it to fire
        ResetEvent(g_hVBlankEvent);
        // Re-check lastVBlank hasn't updated while we reset
        LONGLONG newLastVBlank = g_LastVBlankQPC.load(std::memory_order_acquire);
        if (newLastVBlank == lastVBlankQPC)
        {
            // Wait for next VBlank signal, timeout = 1 full frame + buffer
            WaitForSingleObject(g_hVBlankEvent, BURST_WAIT_MS);
        }
        // VBlank fired (or timed out). Now spinlock to the precise safe zone offset.
        LONGLONG firedVBlankQPC = g_LastVBlankQPC.load(std::memory_order_acquire);
        long long safeZoneQPC = SAFE_ZONE_NS * g_QPCFreq / 1000000000LL;
        LONGLONG targetQPC = firedVBlankQPC + safeZoneQPC;
        LARGE_INTEGER cur;
        do {
            _mm_pause();
            QueryPerformanceCounter(&cur);
        } while (cur.QuadPart < targetQPC);
    }
    else
    {
        // Very close to VBlank - just spinlock
        SleepPreciseNs(waitNs);
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
        Log("[AutoPacer v11] First frame - hook confirmed active");
        Beep(1000, 120);
    }

    // Pace burst frames to VBlank boundaries. Normal frames pass through instantly.
    PaceFrame();

    // Always present with SyncInterval=0 to keep VRR active.
    // Remove ALLOW_TEARING flag - we handle tearing prevention ourselves.
    UINT cleanFlags = Flags & ~DXGI_PRESENT_ALLOW_TEARING;
    return oPresent(pSC, 0, cleanFlags);
}

// ── Hook installation ─────────────────────────────────────────────────────────
static bool InstallHook()
{
    WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AP11_H";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, "AP11_H", "", WS_POPUP, 0,0,8,8,
        nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr) || !sc)
    {
        Logf("[AutoPacer v11] Hook swapchain failed: 0x%08X", (unsigned)hr);
        DestroyWindow(hw); UnregisterClassA("AP11_H", wc.hInstance);
        return false;
    }

    void** vtable = *(void***)sc;
    bool ok = PatchVTable(vtable, 8, (void*)HookedPresent, (void**)&oPresent);

    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hw); UnregisterClassA("AP11_H", wc.hInstance);

    if (ok) Log("[AutoPacer v11] Present hook installed (vtable slot 8)");
    else    Log("[AutoPacer v11] ERROR: VTable patch failed");
    return ok;
}

// ── Init thread ───────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);
    Sleep(2000);

    Log("[AutoPacer v11] Initializing...");

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_QPCFreq = freq.QuadPart;

    g_IntelOutput = FindPrimaryOutput();

    if (g_IntelOutput)
    {
        // Auto-reset event: relay thread sets it on each VBlank
        g_hVBlankEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_hVBlankEvent)
        {
            g_Running.store(true);
            g_hRelayThread = CreateThread(nullptr, 0, VBlankRelayThread, nullptr, 0, nullptr);
            Log("[AutoPacer v11] VBlank relay started");
        }
        else Log("[AutoPacer v11] WARNING: CreateEvent failed");
    }
    else
    {
        Log("[AutoPacer v11] WARNING: Intel output not found. Burst-gap-only mode.");
    }

    if (!InstallHook()) return 1;

    if (g_IntelOutput && g_hVBlankEvent)
    {
        Log("[AutoPacer v11] SUCCESS - VBlank-aware pacing active");
        Log("[AutoPacer v11] Normal frames: zero latency. Burst frames: next VBlank edge.");
        Beep(880, 200);
    }
    else
    {
        Log("[AutoPacer v11] PARTIAL - Burst gap enforcer active (no VBlank sync)");
        Beep(440, 100); Beep(440, 100);
    }

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
        if (g_hVBlankEvent) SetEvent(g_hVBlankEvent); // wake relay thread to exit
        if (g_hRelayThread) { WaitForSingleObject(g_hRelayThread, 2000); CloseHandle(g_hRelayThread); }
        if (g_hVBlankEvent) CloseHandle(g_hVBlankEvent);
        if (g_IntelOutput) g_IntelOutput->Release();
    }
    return TRUE;
}
