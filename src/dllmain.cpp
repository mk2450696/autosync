// AutoPacer v19 - Dynamic VRR Smoother (Software G-Sync Emulator)
//
// Abandons static FPS limits. Instead, it maintains a real-time rolling average
// of the game's actual framerate. It acts as a smart queue, catching burst frames
// from FG mods and spacing them perfectly according to the current natural framerate.
// This feeds a smooth, evenly-paced stream of frames to the Intel iGPU, allowing
// VRR to function flawlessly at any framerate.

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

// ── Logging ───────────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_logCS;
static bool             g_logCSInit = false;
static char             g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg)
{
    OutputDebugStringA("[AutoPacer v19] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (!g_logCSInit) return;
    EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v19] %s\n", msg);
        fclose(fp);
    }
    LeaveCriticalSection(&g_logCS);
}
static void Logf(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── Dynamic Pacer State ───────────────────────────────────────────────────────
static bool g_FirstFrame = true;
static LARGE_INTEGER g_qpcFreq;

// Rolling Average History (tracks the natural unpaced framerate)
const int HISTORY_SIZE = 16;
static double g_DeltaHistory[HISTORY_SIZE];
static int g_HistoryIdx = 0;

static double g_LastArriveTime  = 0.0;
static double g_LastReleaseTime = 0.0;

static double GetTimeMs()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// WinMM timer resolution (using UINT instead of MMRESULT to avoid mmsystem.h dependency)
typedef UINT (WINAPI* timeBeginPeriod_t)(UINT uPeriod);

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

// ── Hooked Present (The Dynamic Smoother) ─────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        QueryPerformanceFrequency(&g_qpcFreq);
        
        HMODULE hWinMM = LoadLibraryA("winmm.dll");
        if (hWinMM) {
            auto tbp = (timeBeginPeriod_t)GetProcAddress(hWinMM, "timeBeginPeriod");
            if (tbp) tbp(1); // 1ms sleep precision
        }

        double now = GetTimeMs();
        g_LastArriveTime = now;
        g_LastReleaseTime = now;

        // Initialize history with a safe baseline (e.g., 60fps = 16.6ms)
        for (int i = 0; i < HISTORY_SIZE; i++) {
            g_DeltaHistory[i] = 16.666;
        }

        g_FirstFrame = false;
        Log("First Present! Dynamic VRR Smoother Active.");
        Beep(1000, 120);
    }

    double now = GetTimeMs();
    
    // 1. Measure the natural gap between frames arriving from the game/mod
    double arrivalDelta = now - g_LastArriveTime;
    g_LastArriveTime = now;

    // Ignore massive load-screen spikes so they don't break the math
    if (arrivalDelta > 5.0 && arrivalDelta < 100.0) {
        g_DeltaHistory[g_HistoryIdx] = arrivalDelta;
        g_HistoryIdx = (g_HistoryIdx + 1) % HISTORY_SIZE;
    }

    // 2. Calculate the dynamic average frametime of the game right now
    double sum = 0.0;
    for (int i = 0; i < HISTORY_SIZE; i++) sum += g_DeltaHistory[i];
    double dynamicTargetInterval = sum / (double)HISTORY_SIZE;

    // 3. Determine exactly when this frame SHOULD be released to the monitor
    double targetReleaseTime = g_LastReleaseTime + dynamicTargetInterval;

    // Safety net: If the game naturally lagged, don't delay it further
    if (now >= targetReleaseTime) {
        targetReleaseTime = now;
    }
    // Safety net: Never delay a frame by more than 20ms to prevent game engine freezing
    else if (targetReleaseTime - now > 20.0) {
        targetReleaseTime = now + 20.0;
    }

    // 4. Smooth Queue: Hold the burst frame until its perfect dynamic timeslot
    if (now < targetReleaseTime) {
        while (true) {
            double t = GetTimeMs();
            if (t >= targetReleaseTime) break;
            
            if (targetReleaseTime - t > 2.0) {
                Sleep(1); // Give CPU back to the Frame Gen mod
            } else {
                YieldProcessor(); // Micro-spin for exact millisecond precision
            }
        }
    }

    // 5. Release to the screen!
    g_LastReleaseTime = GetTimeMs();
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

        Log("DLL loaded - v19 Dynamic VRR Smoother");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}