// ============================================================
//  AutoPacer v8 - Fixed Threshold Burst Catcher
//  for iGPU display + dGPU render + Frame Generation setups
//
//  THE ONLY JOB OF THIS CODE:
//  If a frame arrives less than 5.5ms after the previous one
//  (a Frame Generation micro-burst), hold it until 5.5ms has
//  elapsed. All other frames pass through with ZERO latency.
//
//  WHY THIS WORKS WITHOUT A FEEDBACK LOOP:
//  The threshold (5.5ms) is a FIXED constant. It never changes.
//  We never measure our own output to set our target.
//  At 158fps output: frames arrive every 6.33ms > 5.5ms threshold
//  -> ALL pass immediately, zero impact on normal gameplay.
//  FG burst (3 frames in 2ms): each frame arrives ~0.7ms apart
//  -> each held until 5.5ms -> spread across VBlank intervals
//  -> Intel VRR sees evenly spaced frames -> no tearing.
//
//  WHY PREVIOUS VERSIONS FAILED:
//  v3: WaitForVBlank in present thread, tanked FPS
//  v4: Hard cap (g_LastPresent + target) = FPS limiter, not burst catcher
//  v5/v6/v7: Measured paced output to set target = feedback loop
//  My v2: CreateSwapChain hook crashed with DLSS Enabler proxy
//
//  WHAT WE DO NOT DO:
//  - No CreateSwapChain/CreateSwapChainForHwnd hooks (crashes FG mods)
//  - No dynamic measurement of frame times
//  - No WaitForVBlank in the present thread
//  - No Intel output detection (not needed)
// ============================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <atomic>
#include <MinHook.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#endif

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t oPresent = nullptr;

// ============================================================
//  CONFIGURATION
//  Minimum nanoseconds between frame presentations.
//
//  At 165Hz: one VBlank period = 6,060,606 ns (6.06ms)
//  At 158fps: one frame = 6,329,114 ns (6.33ms) <- ABOVE threshold
//  FG burst frames arrive < 2,000,000 ns (2ms) <- BELOW threshold
//
//  Result: normal frames always pass, bursts always caught.
//  Adjust if needed:
//    5000000 = 5.0ms (tighter, use if tearing persists)
//    6000000 = 6.0ms (stricter ceiling, closer to 165Hz limit)
// ============================================================
static const long long MIN_FRAME_NS = 5500000LL;

// How many ns before the target to switch from timer to spinlock.
// The timer wakes us ~0.5ms early, spinlock covers precision gap.
static const long long SPIN_LEAD_NS = 1500000LL; // 1.5ms

static HANDLE            g_hTimer    = nullptr;
static LARGE_INTEGER     g_qpcFreq   = {};
static std::atomic<long long> g_lastNs{0};
static std::atomic<bool> g_initialized{false};
static volatile bool     g_firstFrame = true;

// ============================================================
//  Logging
// ============================================================
static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, "AutoPacer.log", "a") == 0) {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
    }
}

// ============================================================
//  High-precision nanosecond clock via QPC
//  Split into whole-second and sub-second parts to avoid
//  integer overflow with long long arithmetic.
// ============================================================
static inline long long NowNs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    // (count / freq) * 1e9  +  (count % freq) * 1e9 / freq
    // Both terms fit in long long for any reasonable runtime.
    return (t.QuadPart / g_qpcFreq.QuadPart) * 1000000000LL
         + (t.QuadPart % g_qpcFreq.QuadPart) * 1000000000LL
           / g_qpcFreq.QuadPart;
}

// ============================================================
//  Hybrid wait: coarse high-res timer + fine spinlock.
//
//  waitNs: total nanoseconds we need to wait.
//
//  We sleep (0% CPU) for (waitNs - SPIN_LEAD_NS), then spin
//  for the remaining ~1.5ms for sub-millisecond precision.
//  The spinlock phase is capped at SPIN_LEAD_NS (1.5ms max).
// ============================================================
static void HybridWait(long long waitNs, long long startNs) {
    const long long targetNs = startNs + MIN_FRAME_NS;

    // Coarse phase: use high-res waitable timer if wait > 2ms
    if (waitNs > SPIN_LEAD_NS + 500000LL) { // > 2ms total
        LARGE_INTEGER due;
        // Negative = relative time, units = 100ns intervals
        long long coarseNs = waitNs - SPIN_LEAD_NS;
        due.QuadPart = -(coarseNs / 100LL);
        SetWaitableTimer(g_hTimer, &due, 0, NULL, NULL, 0);
        WaitForSingleObject(g_hTimer, 20); // 20ms safety timeout
    }

    // Fine phase: spinlock for precision
    // Maximum spin duration = SPIN_LEAD_NS = 1.5ms
    // (irrelevant to render thread performance at this scale)
    while (NowNs() < targetNs) {
        YieldProcessor(); // CPU PAUSE instruction, not thread yield
    }
}

// ============================================================
//  Present Hook - The burst catcher
//
//  Called for every frame including FG-generated frames.
//  Two cases:
//  1. Normal frame (elapsed >= 5.5ms): pass immediately, 0 latency
//  2. Burst frame  (elapsed <  5.5ms): wait remainder, then pass
// ============================================================
static HRESULT __stdcall hkPresent(IDXGISwapChain* pSC, UINT syncInterval, UINT flags) {
    if (oPresent == nullptr) return E_FAIL; // Shouldn't happen but guard

    if (g_firstFrame) {
        g_firstFrame = false;
        Log("SUCCESS: AutoPacer v8 active. Burst threshold: 5.5ms");
        Beep(880, 120);
    }

    long long nowNs  = NowNs();
    long long lastNs = g_lastNs.load(std::memory_order_relaxed);

    if (lastNs > 0) {
        long long elapsedNs = nowNs - lastNs;

        if (elapsedNs < MIN_FRAME_NS) {
            // Burst frame detected. Hold until minimum interval elapsed.
            long long waitNs = MIN_FRAME_NS - elapsedNs;
            HybridWait(waitNs, lastNs);
            nowNs = NowNs(); // refresh after wait
        }
        // else: normal frame, falls through immediately
    }

    // Store presentation timestamp
    g_lastNs.store(nowNs, std::memory_order_relaxed);

    // Forward to real Present:
    // - syncInterval = 0: VRR must not be overridden by vsync
    // - Strip DO_NOT_WAIT (we already waited above if needed)
    // - Keep all other flags as the game/mod set them
    UINT outFlags = flags & ~DXGI_PRESENT_DO_NOT_WAIT;
    return oPresent(pSC, 0, outFlags);
}

// ============================================================
//  Initialization Thread
//  Waits for DXGI + proxy mods to load, then installs the hook
//  via a dummy DX11 device vtable lookup.
//
//  WHY DUMMY DEVICE (not CreateSwapChain hook):
//  MinHook patches the actual IDXGISwapChain::Present function
//  code in memory. All IDXGISwapChain instances (game's real one
//  AND any FG proxy) share the same vtable and therefore the
//  same function code address. Patching it via a dummy device's
//  vtable patches it globally - catches every Present call.
//
//  Hooking CreateSwapChain CRASHES because DLSS Enabler already
//  hooks it as a proxy - double-hooking corrupts the call chain.
// ============================================================
static DWORD WINAPI InitThread(LPVOID) {
    // Wait for dxgi.dll to appear (game + proxy mods load it)
    while (!GetModuleHandleA("dxgi.dll")) Sleep(100);

    // Extra delay: let DLSS Enabler / OptiScaler finish patching
    Sleep(2500);

    Log("AutoPacer v8 initializing...");

    // System timer resolution: 1ms for accurate timer scheduling
    timeBeginPeriod(1);

    // High-resolution waitable timer (Windows 10 1803+)
    g_hTimer = CreateWaitableTimerExW(
        NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if (!g_hTimer) {
        // Fallback: standard waitable timer (still better than Sleep)
        g_hTimer = CreateWaitableTimerW(NULL, TRUE, NULL);
        Log("Note: using standard timer (upgrade to Win10 1803+ for best precision)");
    } else {
        Log("High-resolution waitable timer ready.");
    }

    // Create a minimal dummy DX11 device + swapchain
    // Purpose: read vtable[8] address = IDXGISwapChain::Present
    WNDCLASSEXA wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = DefWindowProcA;
    wc.hInstance    = GetModuleHandleA(NULL);
    wc.lpszClassName = "AP8";
    RegisterClassExA(&wc);
    HWND hWnd = CreateWindowA("AP8", "", WS_POPUP,
        0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd    = {};
    sd.BufferCount             = 1;
    sd.BufferDesc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width        = 1;
    sd.BufferDesc.Height       = 1;
    sd.BufferUsage             = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow            = hWnd;
    sd.SampleDesc.Count        = 1;
    sd.Windowed                = TRUE;
    sd.SwapEffect              = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl       = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device*       pDev   = nullptr;
    ID3D11DeviceContext* pCtx  = nullptr;
    IDXGISwapChain*     pSC    = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &sd, &pSC, &pDev, NULL, &pCtx
    );

    if (SUCCEEDED(hr) && pSC) {
        void** vtbl = *reinterpret_cast<void***>(pSC);

        MH_Initialize();
        MH_STATUS s = MH_CreateHook(
            vtbl[8],                              // IDXGISwapChain::Present
            reinterpret_cast<void*>(&hkPresent),
            reinterpret_cast<void**>(&oPresent)
        );

        if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(vtbl[8]);
            Log("Present hook installed. Waiting for first frame...");
            Beep(1000, 150);
        } else {
            char buf[80];
            sprintf_s(buf, "Hook install failed: MH_STATUS = %d", (int)s);
            Log(buf);
        }

        pSC->Release();
        pDev->Release();
        pCtx->Release();
    } else {
        char buf[80];
        sprintf_s(buf, "Dummy device failed: HRESULT = 0x%08X", (unsigned)hr);
        Log(buf);
    }

    DestroyWindow(hWnd);
    UnregisterClassA("AP8", wc.hInstance);
    return 0;
}

// ============================================================
//  DLL Entry Point
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // g_initialized prevents double-injection
        bool expected = false;
        if (g_initialized.compare_exchange_strong(expected, true)) {
            QueryPerformanceFrequency(&g_qpcFreq);
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        }
    }
    return TRUE;
}