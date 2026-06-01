// AutoPacer v16 - Synchronous Minimum Gap Enforcer
//
// ROOT CAUSE (proven by v28 telemetry):
//   NVIDIA renders frames perfectly at ~7.7ms intervals.
//   PCIe CASO transfer adds variable latency, causing frames to "bunch" at
//   Intel's display controller — two frames arriving 0ms apart, then a 14ms
//   starvation gap. Intel drops the first frame (0.000ms display gap) and
//   shows the second. Queue is now empty, monitor starves for 14ms. Judder.
//
// WHY ASYNC QUEUE (v38-v41) FAILED:
//   Frame Generation (DLSS Enabler/OptiScaler) generates interpolated frames
//   using motion vectors derived from the PREVIOUS PRESENTED FRAME.
//   When HookedPresent returns S_OK without calling oPresent, FG thinks the
//   frame was consumed and generates the next frame using stale data.
//   Result: duplicate frames, UI artifacts, flashing. Incompatible with FG.
//
// THE CORRECT APPROACH - Synchronous Gap Enforcement:
//   We stay fully synchronous. Present is intercepted, we enforce a minimum
//   time gap since the last Present, then call oPresent for real.
//   FG sees the real Present call. Its timeline stays intact. No artifacts.
//
// THE MATH:
//   At 165Hz, one scanout = 6.06ms. Frames arriving < 5.5ms apart get bunched.
//   Base frame gap = ~7.7ms (fine, passes through immediately).
//   Generated frame gap = ~1ms (we hold it until total gap = 5.5ms, max 4.5ms wait).
//   4.5ms added latency on generated frames only = imperceptible to humans.
//   Result: Intel never sees two frames < 5.5ms apart. Zero drops. No starvation.
//
// VRR: Fully preserved. ALLOW_TEARING untouched. Frames arrive variable but
//       never bunched. Monitor adapts naturally. No static 165Hz lock.
//
// HOOK METHOD: Dummy D3D11 swapchain vtable patch (proven stable from v28).
//   No factory hooking. No IAT patching. No swapchain modification.
//   Works after all mods have loaded. No conflicts.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdarg.h>
#include <atomic>

// ── Config ────────────────────────────────────────────────────────────────────
// Minimum milliseconds between any two Present calls delivered to Intel.
// Below this threshold we spinwait. Must be < 6.06ms (one 165Hz scanout).
// 5.5ms gives 0.56ms margin. Tested safe range: 5.0 - 5.8ms.
static const double MIN_GAP_MS = 5.5;

// Maximum time we will ever hold a frame waiting (safety valve).
// If a frame is already 8ms late, pass it through regardless.
// This prevents any feedback loop or stall if something external changes.
static const double MAX_HOLD_MS = 8.0;

// ── Logging ───────────────────────────────────────────────────────────────────
static char g_logPath[MAX_PATH] = "AutoPacer.log";
static CRITICAL_SECTION g_logCS;
static bool g_logCSInit = false;

static void Log(const char* msg)
{
    OutputDebugStringA("[AutoPacer v16] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (!g_logCSInit) return;
    EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v16] %s\n", msg);
        fclose(fp);
    }
    LeaveCriticalSection(&g_logCS);
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── Timing ───────────────────────────────────────────────────────────────────
static LARGE_INTEGER g_qpcFreq;

static double GetTimeMs()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)qpc.QuadPart * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── State ─────────────────────────────────────────────────────────────────────
static double g_LastPresentMs  = 0.0;  // When we last called oPresent
static bool   g_FirstFrame     = true;

// Diagnostics logged periodically
static int    g_FrameCount     = 0;
static int    g_HeldFrames     = 0;    // How many frames we delayed
static double g_MaxHoldApplied = 0.0;  // Longest hold we ever applied

// ── VTable patch ──────────────────────────────────────────────────────────────
static bool WritePtr(void** addr, void* newVal, void** oldVal)
{
    DWORD old;
    if (!VirtualProtect(addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (oldVal) *oldVal = *addr;
    *addr = newVal;
    VirtualProtect(addr, sizeof(void*), old, &old);
    return true;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent = nullptr;
static const int   SLOT_Present = 8;

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame     = false;
        g_LastPresentMs  = GetTimeMs();
        Log("First Present intercepted - gap enforcer ACTIVE");
        Logf("Config: MIN_GAP=%.2f ms, MAX_HOLD=%.2f ms", MIN_GAP_MS, MAX_HOLD_MS);
        Beep(1000, 120);
    }

    double now        = GetTimeMs();
    double sinceLastMs = now - g_LastPresentMs;

    // Only enforce the gap if the frame arrived faster than MIN_GAP_MS.
    // Base frames (~7.7ms apart) pass through immediately — zero interference.
    // Generated frames (~1ms apart) are held until MIN_GAP_MS has elapsed.
    if (sinceLastMs < MIN_GAP_MS)
    {
        double waitUntil = g_LastPresentMs + MIN_GAP_MS;
        double deadline  = now + MAX_HOLD_MS; // safety: never wait longer than this

        // Precision spinwait. Uses YieldProcessor to avoid burning a full core.
        // At 4.5ms max hold, this is imperceptible.
        while (true)
        {
            double t = GetTimeMs();
            if (t >= waitUntil) break;
            if (t >= deadline)  break; // safety valve
            YieldProcessor();
            YieldProcessor();
            YieldProcessor();
            YieldProcessor();
        }

        double held = GetTimeMs() - now;
        g_HeldFrames++;
        if (held > g_MaxHoldApplied) g_MaxHoldApplied = held;
    }

    // Call the real Present (or DLSS Enabler's hook, whichever is chained).
    // SyncInterval and Flags pass through COMPLETELY UNCHANGED.
    // ALLOW_TEARING is preserved. VRR stays active.
    g_LastPresentMs = GetTimeMs();
    g_FrameCount++;

    // Periodic diagnostics — logged every 600 frames (~5 seconds at 130fps)
    if (g_FrameCount % 600 == 0)
    {
        double heldPct = g_FrameCount > 0 ? (g_HeldFrames * 100.0 / g_FrameCount) : 0.0;
        Logf("Stats: %d frames | %d held (%.1f%%) | MaxHold=%.3f ms",
             g_FrameCount, g_HeldFrames, heldPct, g_MaxHoldApplied);
    }

    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init Thread ───────────────────────────────────────────────────────────────
// Uses the proven dummy swapchain vtable technique from v28.
// Waits for game window to appear (all mods loaded by then), then hooks.
static DWORD WINAPI InitThread(LPVOID)
{
    // Wait for dxgi.dll
    for (int i = 0; i < 200; ++i) {
        if (GetModuleHandleA("dxgi.dll")) break;
        Sleep(50);
    }
    if (!GetModuleHandleA("dxgi.dll")) { Log("ERROR: dxgi.dll never loaded"); return 1; }

    // Wait for game window — by this point DLSS Enabler/OptiScaler are fully loaded
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
            if ((r.right - r.left) > 400 && title[0] != '\0')
            {
                Logf("Game window found: '%s' (%dx%d)",
                    title, r.right - r.left, r.bottom - r.top);
                break;
            }
        }
    }

    // Extra settle — let all mod hooks finish installing
    Sleep(500);
    Log("Installing Present hook via dummy swapchain...");

    // Create a tiny invisible dummy window + D3D11 device to get the vtable
    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0,
                       GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                       "APDummy16", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("APDummy16", "D", WS_OVERLAPPEDWINDOW,
                                   0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount         = 1;
    sd.BufferDesc.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage         = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow        = dummyWnd;
    sd.SampleDesc.Count    = 1;
    sd.Windowed            = TRUE;
    sd.SwapEffect          = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSC  = nullptr;
    ID3D11Device*   pDev = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &pSC, &pDev, &fl, nullptr);

    if (SUCCEEDED(hr) && pSC)
    {
        // Read slot 8 from the vtable.
        // If DLSS Enabler/OptiScaler already hooked Present, oPresent = their hook.
        // If not, oPresent = original IDXGISwapChain::Present.
        // We become the outermost wrapper in the chain. No conflict possible.
        void** vt = *(void***)pSC;
        if (WritePtr(&vt[SLOT_Present], (void*)HookedPresent, (void**)&oPresent))
            Logf("Present hook installed. Slot 8 was: %p", oPresent);
        else
            Log("ERROR: vtable write failed");

        pSC->Release();
        pDev->Release();
    }
    else
        Logf("ERROR: D3D11CreateDeviceAndSwapChain failed: 0x%08X", (unsigned)hr);

    DestroyWindow(dummyWnd);
    UnregisterClassA("APDummy16", wc.hInstance);

    Log("Init complete. Gap enforcer ready.");
    Beep(880, 150);
    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq);
        InitializeCriticalSection(&g_logCS);
        g_logCSInit = true;

        // Set log path to same directory as the ASI file
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }

        // Clear old log
        FILE* fp;
        if (fopen_s(&fp, g_logPath, "w") == 0) fclose(fp);

        Log("DLL loaded - v16 Synchronous Gap Enforcer");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}
