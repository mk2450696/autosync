// AutoPacer v16 - Dummy Swapchain VTable Hooking
//
// WHY v15 FAILED: We waited 500ms to avoid mod conflicts, which meant the game
//                 had already created its swapchain. Our CreateSwapChain hook missed it.
//
// THE v16 APPROACH:
//   1. Wait 500ms until the game is fully running and all mods have hooked.
//   2. Create a temporary, invisible D3D11 Dummy Swapchain.
//   3. Extract the VTable address from our dummy swapchain. Because COM vtables
//      are shared across the entire process, this is the exact same vtable used
//      by the game's real swapchain.
//   4. Read Slot 8 (Present). It will point to DLSS Enabler's hook or DXGI original.
//   5. Overwrite Slot 8 with our HookedPresent, then destroy our dummy swapchain.
//   6. The next time the game calls Present on its real swapchain, we intercept it!

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
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

// ── State ─────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_PresentHooked { false };
static bool              g_WaitableActive = false;
static bool              g_DwmFallback    = false;
static bool              g_FirstFrame     = true;
static HANDLE            g_hWaitable      = nullptr;

static const UINT  FRAME_LATENCY    = 1;
static const DWORD WAITABLE_TIMEOUT = 33;

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

// ── Function pointer types ────────────────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present oPresent = nullptr;
static const int SLOT_Present = 8;

// ── Waitable / DWM fallback setup ─────────────────────────────────────────────
static void SetupSync(IDXGISwapChain* sc)
{
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
    {
        HANDLE h = sc2->GetFrameLatencyWaitableObject();
        if (h)
        {
            sc2->SetMaximumFrameLatency(FRAME_LATENCY);
            g_hWaitable      = h;
            g_WaitableActive = true;
            Log("Sync mode: WAITABLE OBJECT (pipeline-sync to Intel VBlank)");
        }
        sc2->Release();
    }

    if (!g_WaitableActive)
    {
        g_DwmFallback = true;
        Log("Sync mode: DWM FLUSH fallback (BitBlt swapchain, no waitable available)");
    }
}

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame)
    {
        g_FirstFrame = false;
        Log("First Present intercepted! We have the REAL game swapchain.");
        SetupSync(pSC);
        Logf("  Waitable=%s DwmFallback=%s",
            g_WaitableActive ? "YES" : "NO",
            g_DwmFallback    ? "YES" : "NO");
        Beep(1000, 120);
    }

    if (g_WaitableActive && g_hWaitable)
    {
        DWORD r = WaitForSingleObjectEx(g_hWaitable, WAITABLE_TIMEOUT, FALSE);
        if (r == WAIT_TIMEOUT) Log("WARNING: waitable timeout");
    }
    else if (g_DwmFallback)
    {
        DwmFlush();
    }

    // Call DLSS Enabler / Original Present. 
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

    // Wait for the game window to appear
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

    // Wait for DLSS Enabler and OptiScaler to finish hooking the game
    Sleep(500);
    Log("Mod settle time elapsed. Spawning dummy swapchain to steal VTable.");

    // Create Dummy Window
    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyWindow", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("DummyWindow", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    // Dummy Swapchain Setup
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
        
        // Slot 8 currently contains either Original Present or DLSS Enabler's Present hook
        if (WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent))
        {
            Logf("SUCCESS: Present chain wrap installed via dummy vtable! Slot 8 was: %p", oPresent);
            g_PresentHooked = true;
        }
        else
        {
            Log("ERROR: VTable write failed.");
        }

        // Cleanup the dummy device/swapchain immediately
        pDummySC->Release();
        pDummyDev->Release();
    }
    else
    {
        Logf("ERROR: Dummy swapchain creation failed: 0x%08X", (unsigned)hr);
    }

    // Cleanup Dummy Window
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

        Log("DLL loaded - v16 Dummy Swapchain approach");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_hWaitable) CloseHandle(g_hWaitable);
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}