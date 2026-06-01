// AutoPacer v10 - Software G-Sync for iGPU display + dGPU render (CASO) setups
//
// Problem: Intel UHD 730 drives the display. RTX 3060 Ti renders (CASO Tier 3).
// FG generates frame bursts that arrive mid-scanout on Intel -> tearing.
//
// Fix: Dedicated VBlank relay thread calls WaitForVBlank() on Intel's IDXGIOutput
// (found by matching primary monitor device name). Posts 1 semaphore slot per VBlank.
// Present hook: burst-catch FG frames (6.2ms min), wait on VBlank semaphore,
// then present SyncInterval=0. Frame only swaps at Intel's actual blanking interval.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <atomic>

// ── Configuration ─────────────────────────────────────────────────────────────
static const long long BURST_THRESHOLD_NS = 6200000LL;  // 6.2ms for 165Hz
static const DWORD     VBLANK_TIMEOUT_MS  = 50;
// ──────────────────────────────────────────────────────────────────────────────

static HANDLE            g_hVBlankSem    = nullptr;
static IDXGIOutput*      g_TargetOutput  = nullptr;
static std::atomic<bool> g_Running       { false };
static HANDLE            g_hRelayThread  = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent        = nullptr;
static LONGLONG    g_QPCFreq       = 0;
static LONGLONG    g_LastPresentQPC= 0;
static bool        g_FirstFrame    = true;

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

// ── Find the IDXGIOutput for the primary monitor ───────────────────────────────
//
// Key facts from system enumeration:
//   Primary monitor = \\.\DISPLAY5  (Lenovo Y27q-20, Intel iGPU, DisplayPort)
//   Primary monitor rect = (0,0,2560,1440)
//   Primary HMONITOR = 0x10001
//
// Problem seen in logs: DXGI_ADAPTER_DESC1.VendorId was wrong (all showed 0x10DE).
// This is because DXGI_ADAPTER_DESC1 uses a different struct layout in some SDK
// versions - specifically, the Flags field comes BEFORE VendorId in some headers.
// Solution: don't use VendorId at all. Match purely by output device name and
// monitor coordinates. Skip NVIDIA by checking adapter description string instead.
//
// Also seen: EnumOutputs returned 0 outputs from game process context for some
// adapters. Solution: try BOTH IDXGIFactory1 and IDXGIFactory6, and also try
// getting the output directly from a created swapchain via GetContainingOutput.

static IDXGIOutput* FindPrimaryOutput()
{
    // Get primary monitor info
    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hPrimary, &mi);

    char primaryDeviceName[64] = {};
    strncpy_s(primaryDeviceName, mi.szDevice, 63);

    Logf("[AutoPacer v10] Primary monitor: HMONITOR=0x%p device='%s' rect=(%d,%d,%d,%d)",
        (void*)hPrimary, primaryDeviceName,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right, mi.rcMonitor.bottom);

    IDXGIOutput* result = nullptr;

    // ── Method A: IDXGIFactory1 full enumeration ──────────────────────────────
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        {
            for (UINT ai = 0; !result; ++ai)
            {
                IDXGIAdapter* adapter = nullptr;
                if (FAILED(factory->EnumAdapters(ai, &adapter))) break;

                // Get description via the base IDXGIAdapter (avoids DXGI_ADAPTER_DESC1 layout issues)
                DXGI_ADAPTER_DESC adesc = {};
                adapter->GetDesc(&adesc);

                char adapterName[128] = {};
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, adapterName, 127, nullptr, nullptr);
                Logf("[AutoPacer v10]   [A] Adapter %u: '%s' VendorId=0x%04X", ai, adapterName, adesc.VendorId);

                // Skip NVIDIA (0x10DE) by both name and vendor ID
                bool isNV = (adesc.VendorId == 0x10DE) ||
                            (strstr(adapterName, "NVIDIA") != nullptr) ||
                            (strstr(adapterName, "GeForce") != nullptr);
                // Skip Microsoft software renderer
                bool isSW = (strstr(adapterName, "Microsoft") != nullptr) ||
                            (strstr(adapterName, "Basic Render") != nullptr);

                if (!isNV && !isSW)
                {
                    for (UINT oi = 0; ; ++oi)
                    {
                        IDXGIOutput* output = nullptr;
                        if (FAILED(adapter->EnumOutputs(oi, &output))) break;

                        DXGI_OUTPUT_DESC odesc = {};
                        output->GetDesc(&odesc);

                        char devName[64] = {};
                        WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devName, 63, nullptr, nullptr);
                        Logf("[AutoPacer v10]     [A] Output %u: device='%s' HMON=0x%p coords=(%d,%d,%d,%d)",
                            oi, devName, (void*)odesc.Monitor,
                            odesc.DesktopCoordinates.left, odesc.DesktopCoordinates.top,
                            odesc.DesktopCoordinates.right, odesc.DesktopCoordinates.bottom);

                        bool nameMatch  = (_stricmp(devName, primaryDeviceName) == 0);
                        bool hmonMatch  = (odesc.Monitor == hPrimary);
                        bool coordMatch = (odesc.DesktopCoordinates.left  == mi.rcMonitor.left  &&
                                          odesc.DesktopCoordinates.top   == mi.rcMonitor.top   &&
                                          odesc.DesktopCoordinates.right  == mi.rcMonitor.right &&
                                          odesc.DesktopCoordinates.bottom == mi.rcMonitor.bottom);

                        if (nameMatch || hmonMatch || coordMatch)
                        {
                            Logf("[AutoPacer v10]   MATCH via Method A (name=%d hmon=%d coord=%d): %s output %u",
                                nameMatch, hmonMatch, coordMatch, adapterName, oi);
                            result = output;
                            output = nullptr;
                        }
                        if (output) output->Release();
                        if (result) break;
                    }
                }
                adapter->Release();
            }
            factory->Release();
        }
        else Log("[AutoPacer v10] CreateDXGIFactory1 failed");
    }

    if (result) return result;
    Log("[AutoPacer v10] Method A found nothing, trying Method B (IDXGIFactory6)...");

    // ── Method B: IDXGIFactory6 - enumerate by MinPower preference (finds iGPU first) ──
    {
        IDXGIFactory6* factory6 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&factory6)))
        {
            for (UINT ai = 0; !result; ++ai)
            {
                IDXGIAdapter1* adapter = nullptr;
                // DXGI_GPU_PREFERENCE_MINIMUM_POWER lists iGPU first
                HRESULT hr = factory6->EnumAdapterByGpuPreference(
                    ai, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                    __uuidof(IDXGIAdapter1), (void**)&adapter);
                if (FAILED(hr)) break;

                DXGI_ADAPTER_DESC adesc = {};
                adapter->GetDesc(&adesc);
                char adapterName[128] = {};
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, adapterName, 127, nullptr, nullptr);
                Logf("[AutoPacer v10]   [B] Adapter %u: '%s' VendorId=0x%04X", ai, adapterName, adesc.VendorId);

                bool isNV = (adesc.VendorId == 0x10DE) || (strstr(adapterName, "NVIDIA") != nullptr);
                bool isSW = (strstr(adapterName, "Microsoft") != nullptr) || (strstr(adapterName, "Basic Render") != nullptr);

                if (!isNV && !isSW)
                {
                    for (UINT oi = 0; ; ++oi)
                    {
                        IDXGIOutput* output = nullptr;
                        if (FAILED(adapter->EnumOutputs(oi, &output))) break;

                        DXGI_OUTPUT_DESC odesc = {};
                        output->GetDesc(&odesc);
                        char devName[64] = {};
                        WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devName, 63, nullptr, nullptr);
                        Logf("[AutoPacer v10]     [B] Output %u: device='%s' HMON=0x%p coords=(%d,%d,%d,%d)",
                            oi, devName, (void*)odesc.Monitor,
                            odesc.DesktopCoordinates.left, odesc.DesktopCoordinates.top,
                            odesc.DesktopCoordinates.right, odesc.DesktopCoordinates.bottom);

                        bool nameMatch  = (_stricmp(devName, primaryDeviceName) == 0);
                        bool hmonMatch  = (odesc.Monitor == hPrimary);
                        bool coordMatch = (odesc.DesktopCoordinates.left  == mi.rcMonitor.left  &&
                                          odesc.DesktopCoordinates.top   == mi.rcMonitor.top   &&
                                          odesc.DesktopCoordinates.right  == mi.rcMonitor.right &&
                                          odesc.DesktopCoordinates.bottom == mi.rcMonitor.bottom);
                        if (nameMatch || hmonMatch || coordMatch)
                        {
                            Logf("[AutoPacer v10]   MATCH via Method B: %s output %u", adapterName, oi);
                            result = output; output = nullptr;
                        }
                        if (output) output->Release();
                        if (result) break;
                    }
                }
                adapter->Release();
            }
            factory6->Release();
        }
        else Log("[AutoPacer v10] IDXGIFactory6 not available");
    }

    if (result) return result;
    Log("[AutoPacer v10] Method B found nothing, trying Method C (swapchain GetContainingOutput)...");

    // ── Method C: create a real swapchain on the primary monitor HWND,
    //             call GetContainingOutput() which always returns the correct output ──
    {
        // Find any visible top-level window on the primary monitor to attach to
        // Fall back to desktop HWND
        HWND targetHwnd = nullptr;

        struct FindData { HMONITOR hMon; HWND result; };
        FindData fd = { hPrimary, nullptr };
        EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
            FindData* pfd = (FindData*)lp;
            if (!IsWindowVisible(hw)) return TRUE;
            HMONITOR hm = MonitorFromWindow(hw, MONITOR_DEFAULTTONULL);
            if (hm == pfd->hMon) { pfd->result = hw; return FALSE; }
            return TRUE;
        }, (LPARAM)&fd);

        targetHwnd = fd.result ? fd.result : GetDesktopWindow();
        Logf("[AutoPacer v10]   [C] Using HWND=0x%p for temp swapchain", (void*)targetHwnd);

        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "AP10_C";
        RegisterClassExA(&wc);
        HWND hw = CreateWindowExA(0, "AP10_C", "", WS_POPUP, 0, 0, 8, 8,
            nullptr, nullptr, wc.hInstance, nullptr);

        // Move window to primary monitor
        SetWindowPos(hw, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, 8, 8, SWP_NOZORDER | SWP_NOACTIVATE);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hw; sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

        if (SUCCEEDED(hr) && sc)
        {
            IDXGIOutput* containingOutput = nullptr;
            hr = sc->GetContainingOutput(&containingOutput);
            if (SUCCEEDED(hr) && containingOutput)
            {
                DXGI_OUTPUT_DESC odesc = {};
                containingOutput->GetDesc(&odesc);
                char devName[64] = {};
                WideCharToMultiByte(CP_ACP, 0, odesc.DeviceName, -1, devName, 63, nullptr, nullptr);
                Logf("[AutoPacer v10]   [C] GetContainingOutput: device='%s' HMON=0x%p coords=(%d,%d,%d,%d)",
                    devName, (void*)odesc.Monitor,
                    odesc.DesktopCoordinates.left, odesc.DesktopCoordinates.top,
                    odesc.DesktopCoordinates.right, odesc.DesktopCoordinates.bottom);

                // Accept this output if it's on the primary monitor
                bool nameMatch  = (_stricmp(devName, primaryDeviceName) == 0);
                bool hmonMatch  = (odesc.Monitor == hPrimary);
                bool coordMatch = (odesc.DesktopCoordinates.left == mi.rcMonitor.left &&
                                  odesc.DesktopCoordinates.top  == mi.rcMonitor.top);
                if (nameMatch || hmonMatch || coordMatch)
                {
                    Log("[AutoPacer v10]   MATCH via Method C: GetContainingOutput");
                    result = containingOutput;
                    containingOutput = nullptr;
                }
                if (containingOutput) containingOutput->Release();
            }
            else Logf("[AutoPacer v10]   [C] GetContainingOutput failed: 0x%08X", (unsigned)hr);

            sc->Release(); dev->Release(); ctx->Release();
        }
        else Logf("[AutoPacer v10]   [C] D3D11CreateDeviceAndSwapChain failed: 0x%08X", (unsigned)hr);

        DestroyWindow(hw);
        UnregisterClassA("AP10_C", wc.hInstance);
    }

    if (!result)
    {
        Log("[AutoPacer v10] ERROR: All 3 methods failed to find primary output.");
        Log("[AutoPacer v10] See full log above. The VBlank relay will not start.");
        Log("[AutoPacer v10] AutoPacer will still hook Present but without VBlank sync.");
        Log("[AutoPacer v10] Burst catcher only mode - may reduce but not eliminate tearing.");
    }

    return result;
}

// ── VBlank relay thread ────────────────────────────────────────────────────────
static DWORD WINAPI VBlankRelayThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Log("[AutoPacer v10] VBlank relay thread started");

    while (g_Running.load(std::memory_order_relaxed))
    {
        if (FAILED(g_TargetOutput->WaitForVBlank()))
        {
            Sleep(1);
            continue;
        }
        LONG prev = 0;
        ReleaseSemaphore(g_hVBlankSem, 1, &prev);
        if (prev >= 2)
            WaitForSingleObject(g_hVBlankSem, 0);
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
        if (waitNs > 1500000LL)
        {
            HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
            if (hTimer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -((waitNs - 1000000LL) / 100LL);
                SetWaitableTimerEx(hTimer, &due, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(hTimer, 20);
                CloseHandle(hTimer);
            }
        }
        long long targetQPC = g_LastPresentQPC + (BURST_THRESHOLD_NS * g_QPCFreq / 1000000000LL);
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < targetQPC);
    }
}

// ── Hooked Present ─────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("[AutoPacer v10] First frame - hook confirmed active");
        Beep(1000, 120);
    }

    EnforceBurstThreshold();

    if (g_hVBlankSem)
        WaitForSingleObject(g_hVBlankSem, VBLANK_TIMEOUT_MS);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastPresentQPC = now.QuadPart;

    UINT cleanFlags = Flags & ~DXGI_PRESENT_ALLOW_TEARING;
    return oPresent(pSC, 0, cleanFlags);
}

// ── Hook installation ──────────────────────────────────────────────────────────
static bool InstallHook()
{
    WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AP10_H";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, "AP10_H", "", WS_POPUP, 0,0,8,8, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr) || !sc)
    {
        Logf("[AutoPacer v10] Hook swapchain failed: 0x%08X", (unsigned)hr);
        DestroyWindow(hw); UnregisterClassA("AP10_H", wc.hInstance);
        return false;
    }

    void** vtable = *(void***)sc;
    bool ok = PatchVTable(vtable, 8, (void*)HookedPresent, (void**)&oPresent);

    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hw); UnregisterClassA("AP10_H", wc.hInstance);

    if (ok) Log("[AutoPacer v10] Present hook installed (vtable slot 8)");
    else    Log("[AutoPacer v10] ERROR: VTable patch failed");
    return ok;
}

// ── Init thread ────────────────────────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);
    Sleep(2000);

    Log("[AutoPacer v10] Initializing...");

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_QPCFreq = freq.QuadPart;

    g_TargetOutput = FindPrimaryOutput();

    if (g_TargetOutput)
    {
        g_hVBlankSem = CreateSemaphoreW(nullptr, 0, 3, nullptr);
        if (g_hVBlankSem)
        {
            g_Running.store(true);
            g_hRelayThread = CreateThread(nullptr, 0, VBlankRelayThread, nullptr, 0, nullptr);
            Log("[AutoPacer v10] VBlank relay started - full Software G-Sync mode");
        }
    }
    else
    {
        Log("[AutoPacer v10] WARNING: Running in burst-catcher-only mode (no VBlank relay)");
        Log("[AutoPacer v10] This reduces tearing but cannot fully eliminate it.");
    }

    if (!InstallHook()) return 1;

    if (g_TargetOutput)
    {
        Log("[AutoPacer v10] SUCCESS - Software G-Sync fully active");
        Beep(880, 200);
    }
    else
    {
        Log("[AutoPacer v10] PARTIAL - Burst catcher active, VBlank relay inactive");
        Beep(440, 100); Beep(440, 100);
    }

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
        if (g_TargetOutput) g_TargetOutput->Release();
    }
    return TRUE;
}
