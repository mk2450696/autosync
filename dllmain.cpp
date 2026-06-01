// AutoPacer v10 - Software G-Sync for iGPU display + dGPU render (CASO) setups
// Architecture:
//   1. Find Intel's IDXGIOutput by matching primary monitor coordinates {0,0}
//   2. Dedicated VBlank thread: WaitForVBlank on Intel output, posts semaphore per VBlank
//   3. Present hook: burst-catches FG frames (6.2ms min spacing), waits on VBlank semaphore,
//      then presents with SyncInterval=0 (VRR-compatible, timed to actual Intel VBlank)
// Result: tear-free at any FPS within VRR range, no judder, no latency overhead

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono;

// ── Configuration ────────────────────────────────────────────────────────────
static constexpr double BURST_THRESHOLD_MS  = 6.2;   // Min ms between frames (just above 165Hz VBlank 6.06ms)
static constexpr DWORD  VBLANK_WAIT_TIMEOUT = 50;     // ms before we give up waiting for VBlank semaphore
static constexpr bool   ENABLE_BEEP         = true;   // Audible init confirmation
// ─────────────────────────────────────────────────────────────────────────────

// ── Globals ───────────────────────────────────────────────────────────────────
static HANDLE               g_hVBlankSem     = nullptr;  // VBlank relay semaphore
static ComPtr<IDXGIOutput>  g_IntelOutput    = nullptr;  // Intel's IDXGIOutput (primary monitor)
static std::atomic<bool>    g_Running        { false };
static std::thread          g_VBlankThread;

// Present hook state
using PFN_Present  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present  oPresent  = nullptr;
static PFN_Present1 oPresent1 = nullptr;

static LONGLONG g_LastPresentQPC = 0;
static LONGLONG g_QPCFreq        = 0;

// ── VTable patching helpers ───────────────────────────────────────────────────
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

// ── Find Intel IDXGIOutput by primary monitor position ────────────────────────
// Primary monitor always has virtual screen position {0,0}.
// DWM confirmed running on Intel, so Intel owns the primary monitor.
static ComPtr<IDXGIOutput> FindIntelOutput()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())))
        return nullptr;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(hPrimary, &mi);

    for (UINT adapterIdx = 0; ; ++adapterIdx)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(adapterIdx, adapter.GetAddressOf())))
            break;

        DXGI_ADAPTER_DESC1 adesc;
        adapter->GetDesc1(&adesc);

        // Skip Microsoft Basic Render Driver and software adapters
        if (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        // Check each output on this adapter
        for (UINT outIdx = 0; ; ++outIdx)
        {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(outIdx, output.GetAddressOf())))
                break;

            DXGI_OUTPUT_DESC odesc;
            output->GetDesc(&odesc);

            // Match by monitor handle
            if (odesc.Monitor == hPrimary)
            {
                // Extra sanity: VendorId 0x8086 = Intel
                if (adesc.VendorId == 0x8086)
                {
                    char adapterName[256] = {};
                    WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, adapterName, 255, nullptr, nullptr);
                    char msg[512];
                    sprintf_s(msg, "[AutoPacer v10] Found Intel output: adapter=%s, output=%d\n", adapterName, outIdx);
                    OutputDebugStringA(msg);
                    return output;
                }
            }
        }
    }

    // Fallback: return the output whose DesktopCoordinates match the primary monitor rect
    // (covers cases where VendorId check might miss something unusual)
    for (UINT adapterIdx = 0; ; ++adapterIdx)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(adapterIdx, adapter.GetAddressOf())))
            break;

        DXGI_ADAPTER_DESC1 adesc;
        adapter->GetDesc1(&adesc);
        if (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (adesc.VendorId == 0x10DE) // Skip NVIDIA in fallback
            continue;

        for (UINT outIdx = 0; ; ++outIdx)
        {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(outIdx, output.GetAddressOf())))
                break;

            DXGI_OUTPUT_DESC odesc;
            output->GetDesc(&odesc);
            if (odesc.DesktopCoordinates.left == mi.rcMonitor.left &&
                odesc.DesktopCoordinates.top  == mi.rcMonitor.top)
            {
                OutputDebugStringA("[AutoPacer v10] Found Intel output via coordinate fallback\n");
                return output;
            }
        }
    }

    OutputDebugStringA("[AutoPacer v10] ERROR: Could not find Intel output\n");
    return nullptr;
}

// ── VBlank relay thread ───────────────────────────────────────────────────────
// Calls WaitForVBlank on Intel output in a tight loop.
// Posts ONE semaphore slot per VBlank.
// This thread is decoupled from the render thread - it NEVER touches Present.
static void VBlankRelayThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    OutputDebugStringA("[AutoPacer v10] VBlank relay thread started\n");

    while (g_Running.load(std::memory_order_relaxed))
    {
        HRESULT hr = g_IntelOutput->WaitForVBlank();
        if (SUCCEEDED(hr))
        {
            // Post one slot - Present hook consumes it
            // ReleaseSemaphore with count 1: if semaphore already has a slot waiting,
            // cap at 2 to avoid queue buildup during low-FPS scenes
            LONG prevCount = 0;
            ReleaseSemaphore(g_hVBlankSem, 1, &prevCount);
            // If prevCount >= 2, drain the extra to prevent frame queue pileup
            if (prevCount >= 2)
                WaitForSingleObject(g_hVBlankSem, 0);
        }
        else
        {
            // WaitForVBlank failed - adapter lost or something wrong
            // Sleep briefly and retry
            Sleep(1);
        }
    }

    OutputDebugStringA("[AutoPacer v10] VBlank relay thread exiting\n");
}

// ── Burst catcher: enforce minimum inter-frame spacing ───────────────────────
// Returns true if we should proceed with present, false if something went wrong.
static void EnforceBurstThreshold()
{
    if (g_QPCFreq == 0 || g_LastPresentQPC == 0)
        return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double elapsedMs = (double)(now.QuadPart - g_LastPresentQPC) * 1000.0 / (double)g_QPCFreq;

    if (elapsedMs < BURST_THRESHOLD_MS)
    {
        double waitMs = BURST_THRESHOLD_MS - elapsedMs;
        // Coarse wait via waitable timer for anything > 1.5ms
        if (waitMs > 1.5)
        {
            HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (hTimer)
            {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -(LONGLONG)((waitMs - 1.5) * 10000.0); // 100ns units, negative = relative
                SetWaitableTimerEx(hTimer, &dueTime, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(hTimer, (DWORD)(waitMs + 2));
                CloseHandle(hTimer);
            }
        }
        // Spinlock for final <= 1.5ms precision
        do {
            QueryPerformanceCounter(&now);
            elapsedMs = (double)(now.QuadPart - g_LastPresentQPC) * 1000.0 / (double)g_QPCFreq;
            if (elapsedMs >= BURST_THRESHOLD_MS) break;
            _mm_pause();
        } while (true);
    }
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // 1. Enforce burst threshold (catches FG micro-bursts)
    EnforceBurstThreshold();

    // 2. Wait for Intel VBlank signal
    //    This is the core of software G-Sync:
    //    we only present when Intel says the blanking interval has started.
    if (g_hVBlankSem && g_IntelOutput)
    {
        WaitForSingleObject(g_hVBlankSem, VBLANK_WAIT_TIMEOUT);
    }

    // 3. Record this present timestamp
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastPresentQPC = now.QuadPart;

    // 4. Always present with SyncInterval=0 (VRR-compatible, we handle the VBlank timing above)
    //    Strip DXGI_PRESENT_ALLOW_TEARING from flags - not needed, we're VBlank-aligned
    UINT cleanFlags = Flags & ~DXGI_PRESENT_ALLOW_TEARING;
    return oPresent(pSwapChain, 0, cleanFlags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags,
                                                  const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    EnforceBurstThreshold();

    if (g_hVBlankSem && g_IntelOutput)
        WaitForSingleObject(g_hVBlankSem, VBLANK_WAIT_TIMEOUT);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastPresentQPC = now.QuadPart;

    UINT cleanFlags = Flags & ~DXGI_PRESENT_ALLOW_TEARING;
    return oPresent1(pSwapChain, 0, cleanFlags, pPresentParameters);
}

// ── Hook installation via temporary swapchain ─────────────────────────────────
static bool InstallPresentHook()
{
    // Create a minimal D3D11 device + swapchain just to get the vtable
    // This works from any process, including DX12 games (DXGI vtable is stable across API versions)
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) hwnd = GetDesktopWindow();

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount       = 2;
    scd.BufferDesc.Width  = 8;
    scd.BufferDesc.Height = 8;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain> tempSC;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &scd, tempSC.GetAddressOf(),
        device.GetAddressOf(), nullptr, ctx.GetAddressOf());

    if (FAILED(hr))
    {
        OutputDebugStringA("[AutoPacer v10] Failed to create temp swapchain for vtable hook\n");
        return false;
    }

    void** vtable = *(void***)tempSC.Get();

    // Present is slot 8, Present1 is slot 22 on IDXGISwapChain1
    bool ok = PatchVTable(vtable, 8, (void*)HookedPresent, (void**)&oPresent);

    ComPtr<IDXGISwapChain1> tempSC1;
    if (SUCCEEDED(tempSC.As(&tempSC1)))
    {
        void** vtable1 = *(void***)tempSC1.Get();
        PatchVTable(vtable1, 22, (void*)HookedPresent1, (void**)&oPresent1);
    }

    return ok;
}

// ── Init / Shutdown ───────────────────────────────────────────────────────────
static void Init()
{
    // QPC frequency
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_QPCFreq = freq.QuadPart;

    // Find Intel output
    g_IntelOutput = FindIntelOutput();
    if (!g_IntelOutput)
    {
        OutputDebugStringA("[AutoPacer v10] FATAL: Intel output not found. Check primary display assignment.\n");
        MessageBoxA(nullptr,
            "AutoPacer v10: Could not find Intel iGPU output.\n\n"
            "Make sure your monitor is connected to the motherboard (iGPU) HDMI port\n"
            "and that the iGPU display is set as the Primary Display in Windows.",
            "AutoPacer v10 Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Create VBlank semaphore (max count 3 to handle burst buffering)
    g_hVBlankSem = CreateSemaphoreW(nullptr, 0, 3, nullptr);
    if (!g_hVBlankSem)
    {
        OutputDebugStringA("[AutoPacer v10] FATAL: Failed to create VBlank semaphore\n");
        return;
    }

    // Start VBlank relay thread
    g_Running.store(true);
    g_VBlankThread = std::thread(VBlankRelayThread);

    // Install Present hook
    if (!InstallPresentHook())
    {
        OutputDebugStringA("[AutoPacer v10] FATAL: Present hook installation failed\n");
        return;
    }

    OutputDebugStringA("[AutoPacer v10] SUCCESS: AutoPacer v10 active. Software G-Sync engaged.\n");

    if (ENABLE_BEEP)
        Beep(1000, 120);
}

static void Shutdown()
{
    g_Running.store(false);
    // Wake the VBlank thread so it can exit
    if (g_hVBlankSem)
        ReleaseSemaphore(g_hVBlankSem, 1, nullptr);
    if (g_VBlankThread.joinable())
        g_VBlankThread.join();
    if (g_hVBlankSem)
    {
        CloseHandle(g_hVBlankSem);
        g_hVBlankSem = nullptr;
    }
    g_IntelOutput.Reset();
    OutputDebugStringA("[AutoPacer v10] Shutdown complete.\n");
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Init on a new thread to avoid DllMain deadlocks
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(500); // Brief wait for game's own DXGI init to complete
            Init();
            return 0;
        }, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        Shutdown();
        break;
    }
    return TRUE;
}
