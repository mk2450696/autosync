// AutoPacer v17 - Asynchronous Smart Pacer
//
// Hooks the real swapchain via the dummy vtable method (proven successful in v16).
// Replaces the Waitable Object with a high-precision QPC (QueryPerformanceCounter)
// time-spacer. It detects FG burst frames and spaces them exactly halfway between
// the base frames, restoring smooth VRR cadence without adding base frame input lag.

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

// ── Logging ───────────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_logCS;
static bool             g_logCSInit = false;
static char             g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg)
{
    OutputDebugStringA("[AutoPacer v17] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (!g_logCSInit) return;
    EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v17] %s\n", msg);
        fclose(fp);
    }
    LeaveCriticalSection(&g_logCS);
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── State & Pacing Variables ──────────────────────────────────────────────────
static std::atomic<bool> g_PresentHooked { false };
static bool              g_FirstFrame    = true;

// High Precision Timing
static LARGE_INTEGER g_qpcFreq;
static double        g_LastPresentTime = 0.0;
static double        g_LastBaseTime    = 0.0;
static double        g_BaseInterval    = 16.666; // Assume 60fps start

static double GetTimeMs()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

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

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent = nullptr;
static const int SLOT_Present = 8;

// ── Hooked Present (The Smart Pacer) ──────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        QueryPerformanceFrequency(&g_qpcFreq);
        g_LastPresentTime = GetTimeMs();
        g_LastBaseTime = g_LastPresentTime;
        g_FirstFrame = false;
        
        Log("First Present intercepted! Smart Pacer is now active.");
        Beep(1000, 120);
    }

    double currentTime = GetTimeMs();
    double timeSinceLast = currentTime - g_LastPresentTime;

    // Detect FG burst frames (arriving less than 3.5ms after the previous frame)
    if (timeSinceLast < 3.5)
    {
        // Target time is exactly halfway between the last base frame and the expected next base frame
        double targetTime = g_LastBaseTime + (g_BaseInterval / 2.0);
        
        // Safety clamp: don't delay more than 16ms to avoid aggressive stuttering
        if (targetTime - currentTime > 16.0) targetTime = currentTime + 16.0;

        // Spin-yield loop (ultra low latency, high precision wait)
        while (GetTimeMs() < targetTime) {
            YieldProcessor(); 
        }

        g_LastPresentTime = GetTimeMs();
    }
    else
    {
        // This is a normal Base Frame.
        double currentBaseInterval = currentTime - g_LastBaseTime;
        
        // Smooth the average interval (clamp between 6ms and 33ms to ignore menu spikes/stutters)
        if (currentBaseInterval > 6.0 && currentBaseInterval < 33.0) {
            g_BaseInterval = (g_BaseInterval * 0.8) + (currentBaseInterval * 0.2);
        }
        
        g_LastBaseTime = currentTime;
        g_LastPresentTime = currentTime;
    }

    // Call DLSS Enabler / Original Present
    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init: Create Dummy Swapchain to Steal VTable ──────────────────────────────
static DWORD WINAPI InitThread(LPVOID)
{
    for (int i = 0; i < 200; ++i) {
        if (GetModuleHandleA("dxgi.dll")) break;
        Sleep(50);
    }
    if (!GetModuleHandleA("dxgi.dll")) { Log("ERROR: dxgi.dll never loaded"); return 1; }
    Log("dxgi.dll present");

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
            if ((r.right - r.left) > 400 && title[0] != '\0')
            {
                gameWnd = fg;
                Logf("Game window found: '%s'", title);
                break;
            }
        }
    }

    // Wait for DLSS Enabler and OptiScaler to finish hooking
    Sleep(500);
    Log("Mod settle time elapsed. Spawning dummy swapchain to steal VTable.");

    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyWindow", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("DummyWindow", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pDummySC = nullptr;
    ID3D11Device* pDummyDev = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &pDummySC, &pDummyDev, &featureLevel, nullptr);

    if (SUCCEEDED(hr) && pDummySC)
    {
        void** vtable = *(void***)pDummySC;
        
        if (WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent))
        {
            Logf("SUCCESS: Present chain wrap installed! Slot 8 was: %p", oPresent);
            g_PresentHooked = true;
        }
        else Log("ERROR: VTable write failed.");

        pDummySC->Release();
        pDummyDev->Release();
    }
    else Logf("ERROR: Dummy swapchain creation failed: 0x%08X", (unsigned)hr);

    DestroyWindow(dummyWnd);
    UnregisterClassA("DummyWindow", wc.hInstance);

    Log("InitThread finished. Waiting for next real frame present...");
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

        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }

        Log("DLL loaded - v17 Smart Pacer approach");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}