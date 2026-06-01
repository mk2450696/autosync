// AutoPacer v10 - Software G-Sync for iGPU display + dGPU render (CASO) setups
//
// Problem: Intel UHD 730 drives the display. RTX 3060 Ti renders (CASO Tier 3).
// FG generates frame bursts that arrive mid-scanout on Intel -> tearing.
//
// Fix: Dedicated VBlank relay thread calls WaitForVBlank() on Intel's IDXGIOutput
// (found by matching primary monitor HWND). Posts 1 semaphore slot per VBlank.
// Present hook: burst-catch FG frames (6.2ms min), wait on VBlank semaphore,
// then present SyncInterval=0. Frame only swaps at Intel's actual blanking interval.
//
// SyncInterval=0 keeps VRR active. Variable FPS -> VRR adapts -> no judder.
// VBlank-aligned present -> no mid-scanout buffer swap -> no tearing.
// This is software G-Sync.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <atomic>
#include <thread>

// ── Configuration ─────────────────────────────────────────────────────────────
// Minimum nanoseconds between frames. Must be >= one VBlank period.
// 165Hz: 6,060,606 ns  -> 6,200,000 (slight margin above VBlank period)
// 144Hz: 6,944,444 ns  -> 7,100,000
// 120Hz: 8,333,333 ns  -> 8,500,000
static const long long BURST_THRESHOLD_NS = 6200000LL;  // 6.2ms for 165Hz

// How long to wait on VBlank semaphore before giving up and presenting anyway.
// 50ms = safe fallback if relay thread stalls (prevents game freeze).
static const DWORD VBLANK_TIMEOUT_MS = 50;
// ──────────────────────────────────────────────────────────────────────────────

// ── VBlank relay globals ───────────────────────────────────────────────────────
static HANDLE           g_hVBlankSem  = nullptr;
static IDXGIOutput*     g_IntelOutput = nullptr;
static std::atomic<bool> g_Running   { false };
static HANDLE           g_hRelayThread = nullptr;

// ── Present hook globals ───────────────────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent = nullptr;

// QPC for burst threshold
static LONGLONG g_QPCFreq    = 0;
static LONGLONG g_LastPresentQPC = 0;

// ── Logging ────────────────────────────────────────────────────────────────────
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

// ── VTable patch (no MinHook dependency) ──────────────────────────────────────
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

// ── Find Intel IDXGIOutput via primary monitor HWND ───────────────────────────
// DWM confirmed on Intel (iGPU), Intel owns primary monitor.
// Match IDXGIOutput by monitor handle from primary monitor.
static IDXGIOutput* FindIntelOutput()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        return nullptr;

    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    IDXGIOutput* result = nullptr;

    for (UINT ai = 0; !result; ++ai)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (FAILED(factory->EnumAdapters1(ai, &adapter))) break;

        DXGI_ADAPTER_DESC1 adesc;
        adapter->GetDesc1(&adesc);

        // Skip software adapters (Microsoft Basic Render Driver, etc.)
        if (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter->Release();
            continue;
        }

        for (UINT oi = 0; ; ++oi)
        {
            IDXGIOutput* output = nullptr;
            if (FAILED(adapter->EnumOutputs(oi, &output))) break;

            DXGI_OUTPUT_DESC odesc;
            output->GetDesc(&odesc);

            // Primary match: monitor handle + vendor is Intel (0x8086)
            if (odesc.Monitor == hPrimary && adesc.VendorId == 0x8086)
            {
                char buf[256];
                char name[128] = {};
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, name, 127, nullptr, nullptr);
                sprintf_s(buf, "[AutoPacer v10] Intel output found: %s (adapter %u, output %u)", name, ai, oi);
                Log(buf);
                result = output; // addref held
                break;
            }
            output->Release();
        }
        adapter->Release();
    }

    // Fallback: any non-NVIDIA, non-software adapter whose output has the primary monitor
    if (!result)
    {
        Log("[AutoPacer v10] Primary match failed, trying coordinate fallback...");
        MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
        GetMonitorInfoA(hPrimary, &mi);

        for (UINT ai = 0; !result; ++ai)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(ai, &adapter))) break;

            DXGI_ADAPTER_DESC1 adesc;
            adapter->GetDesc1(&adesc);

            if ((adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || adesc.VendorId == 0x10DE)
            {
                adapter->Release();
                continue;
            }

            for (UINT oi = 0; ; ++oi)
            {
                IDXGIOutput* output = nullptr;
                if (FAILED(adapter->EnumOutputs(oi, &output))) break;

                DXGI_OUTPUT_DESC odesc;
                output->GetDesc(&odesc);

                if (odesc.DesktopCoordinates.left == mi.rcMonitor.left &&
                    odesc.DesktopCoordinates.top  == mi.rcMonitor.top)
                {
                    Log("[AutoPacer v10] Intel output found via coordinate fallback");
                    result = output;
                    break;
                }
                output->Release();
            }
            adapter->Release();
        }
    }

    factory->Release();

    if (!result)
        Log("[AutoPacer v10] ERROR: Could not find Intel output! Check primary display settings.");

    return result;
}

// ── VBlank relay thread ────────────────────────────────────────────────────────
// Loops WaitForVBlank on Intel output. Posts 1 semaphore slot per VBlank.
// Completely decoupled from render thread - never touches Present.
static DWORD WINAPI VBlankRelayThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Log("[AutoPacer v10] VBlank relay thread started");

    while (g_Running.load(std::memory_order_relaxed))
    {
        if (FAILED(g_IntelOutput->WaitForVBlank()))
        {
            Sleep(1);
            continue;
        }

        // Post one slot. Cap semaphore at 2 to prevent queue buildup.
        LONG prev = 0;
        ReleaseSemaphore(g_hVBlankSem, 1, &prev);
        if (prev >= 2)
            WaitForSingleObject(g_hVBlankSem, 0); // drain extra
    }

    Log("[AutoPacer v10] VBlank relay thread exiting");
    return 0;
}

// ── Burst threshold enforcement ────────────────────────────────────────────────
static void EnforceBurstThreshold()
{
    if (!g_QPCFreq || !g_LastPresentQPC) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    long long elapsedNs = (now.QuadPart - g_LastPresentQPC) * 1000000000LL / g_QPCFreq;

    if (elapsedNs < BURST_THRESHOLD_NS)
    {
        long long waitNs = BURST_THRESHOLD_NS - elapsedNs;

        // Coarse: high-res waitable timer for bulk of wait (leaves 1ms for spinlock)
        if (waitNs > 1500000LL)
        {
            HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
            if (hTimer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -((waitNs - 1000000LL) / 100LL); // 100ns units, negative=relative
                SetWaitableTimerEx(hTimer, &due, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(hTimer, 20);
                CloseHandle(hTimer);
            }
        }

        // Fine: spinlock for final <=1ms
        long long targetQPC = g_LastPresentQPC + (BURST_THRESHOLD_NS * g_QPCFreq / 1000000000LL);
        do {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < targetQPC);
    }
}

// ── Hooked Present ─────────────────────────────────────────────────────────────
static bool g_FirstFrame = true;

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("[AutoPacer v10] First frame - hook confirmed active");
        Beep(1000, 120);
    }

    // 1. Catch FG burst frames
    EnforceBurstThreshold();

    // 2. Wait for Intel VBlank signal from relay thread
    if (g_hVBlankSem)
        WaitForSingleObject(g_hVBlankSem, VBLANK_TIMEOUT_MS);

    // 3. Record timestamp
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastPresentQPC = now.QuadPart;

    // 4. Present with SyncInterval=0 (VRR-compatible), VBlank-aligned
    //    Strip ALLOW_TEARING - not needed, we're already VBlank-aligned
    UINT cleanFlags = Flags & ~DXGI_PRESENT_ALLOW_TEARING;
    return oPresent(pSC, 0, cleanFlags);
}

// ── Hook installation ──────────────────────────────────────────────────────────
static bool InstallHook()
{
    // Create a minimal swapchain to read vtable[8] = Present
    WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AP10_WC";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, "AP10_WC", "", WS_POPUP, 0,0,1,1, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr) || !sc)
    {
        char buf[64]; sprintf_s(buf, "[AutoPacer v10] Dummy swapchain failed: 0x%08X", (unsigned)hr);
        Log(buf);
        DestroyWindow(hw); UnregisterClassA("AP10_WC", wc.hInstance);
        return false;
    }

    void** vtable = *(void***)sc;
    bool ok = PatchVTable(vtable, 8, (void*)HookedPresent, (void**)&oPresent);

    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hw); UnregisterClassA("AP10_WC", wc.hInstance);

    if (ok) Log("[AutoPacer v10] Present hook installed (vtable slot 8)");
    else    Log("[AutoPacer v10] ERROR: VTable patch failed");
    return ok;
}

// ── Init thread ────────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    // Wait for dxgi.dll and game to settle
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);
    Sleep(2000);

    Log("[AutoPacer v10] Initializing...");

    // QPC frequency
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_QPCFreq = freq.QuadPart;

    // Find Intel output
    g_IntelOutput = FindIntelOutput();
    if (!g_IntelOutput)
    {
        MessageBoxA(nullptr,
            "AutoPacer v10: Could not find Intel iGPU output.\n\n"
            "Ensure your monitor is on the motherboard HDMI port\n"
            "and the iGPU display is set as Primary in Windows.",
            "AutoPacer v10", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create VBlank semaphore
    g_hVBlankSem = CreateSemaphoreW(nullptr, 0, 3, nullptr);
    if (!g_hVBlankSem) { Log("[AutoPacer v10] Semaphore creation failed"); return 1; }

    // Start relay thread
    g_Running.store(true);
    g_hRelayThread = CreateThread(nullptr, 0, VBlankRelayThread, nullptr, 0, nullptr);

    // Install Present hook
    if (!InstallHook()) return 1;

    Log("[AutoPacer v10] SUCCESS - Software G-Sync active");
    Beep(880, 200);
    return 0;
}

// ── DllMain ────────────────────────────────────────────────────────────────────
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
        if (g_hVBlankSem) { ReleaseSemaphore(g_hVBlankSem, 1, nullptr); CloseHandle(g_hVBlankSem); }
        if (g_hRelayThread) { WaitForSingleObject(g_hRelayThread, 2000); CloseHandle(g_hRelayThread); }
        if (g_IntelOutput) g_IntelOutput->Release();
    }
    return TRUE;
}
