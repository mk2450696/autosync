// AutoPacer v20 - Asynchronous Hardware Queue Delegate
//
// Completely abandons CPU-based sleeping and thread-blocking (which caused 
// 48 FPS locks and infinite loading screens by starving the game engine).
// Instead, it measures frame arrival times instantly. Base frames are passed
// with VRR (ALLOW_TEARING) intact. Burst frames (from Frame Gen) have the tearing 
// flag stripped, forcing the Intel DWM compositor to hardware-queue them to the 
// next VBlank, preventing tearing natively without ever blocking the CPU thread.

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
    OutputDebugStringA("[AutoPacer v20] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    if (!g_logCSInit) return;
    EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v20] %s\n", msg);
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
static bool g_FirstFrame = true;
static LARGE_INTEGER g_qpcFreq;
static double g_LastPresentTime = 0.0;

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

// ── Hooked Present (Hardware Delegate) ────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        QueryPerformanceFrequency(&g_qpcFreq);
        g_LastPresentTime = GetTimeMs();
        g_FirstFrame = false;
        
        Log("First Present! Asynchronous Hardware Pacer Active.");
        Beep(1000, 120);
    }

    double now = GetTimeMs();
    double gap = now - g_LastPresentTime;
    g_LastPresentTime = now;

    UINT finalFlags = Flags;
    UINT finalSync = SyncInterval;

    // If the frame arrives less than 4ms after the previous one, it is a Frame Gen burst.
    // We strip the ALLOW_TEARING flag. This returns control to the mod instantly (0ms delay),
    // but forces the Intel display driver to lock this specific frame to the next VBlank
    // instead of tearing the screen.
    if (gap < 4.0)
    {
        finalFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
        finalSync = 0; // Ensure DXGI doesn't CPU-block us natively
    }

    // Call the original Present instantly. No sleeps. No loops.
    return oPresent(pSC, finalSync, finalFlags);
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

    Sleep(500);
    Log("Spawning dummy swapchain to steal VTable.");

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
        if (WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent)) {
            Logf("SUCCESS: Present chain wrap installed! Slot 8 was: %p", oPresent);
        }
        pDummySC->Release();
        pDummyDev->Release();
    }

    DestroyWindow(dummyWnd);
    UnregisterClassA("DummyWindow", wc.hInstance);
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

        Log("DLL loaded - v20 Hardware Queue Delegate");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}