// AutoPacer v34 - Deep Telemetry & The 5.5ms Minimum Gap Enforcer
//
// 1. Implements a global Exception Handler to catch and log crashes.
// 2. Extracts and logs deep DXGI swapchain telemetry upon the first Present.
// 3. Implements the strict 5.5ms Minimum Gap Enforcer. If a frame arrives less 
//    than 5.5ms after the previous one, it holds the thread. This physically 
//    prevents the PCIe "double arrival" (0.000ms gaps) while preserving VRR.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <stdio.h>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

// ── Logging System ────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_logCS;
static bool g_logCSInit = false;

static void Log(const char* msg) {
    if (g_logCSInit) EnterCriticalSection(&g_logCS);
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v34] %s\n", msg);
        fclose(fp);
    }
    if (g_logCSInit) LeaveCriticalSection(&g_logCS);
}

static void Logf(const char* fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
}

// ── Global Crash Handler ──────────────────────────────────────────────────────
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    DWORD exceptionCode = pExceptionInfo->ExceptionRecord->ExceptionCode;
    PVOID exceptionAddress = pExceptionInfo->ExceptionRecord->ExceptionAddress;
    Logf("FATAL CRASH DETECTED! Exception Code: 0x%08X at Address: %p", exceptionCode, exceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── Pacing State ──────────────────────────────────────────────────────────────
static bool g_FirstFrame = true;
static LARGE_INTEGER g_qpcFreq;
static double g_LastReleaseTime = 0.0;
const double MINIMUM_GAP_MS = 5.5; // Strictly under 6.06ms (165Hz) to preserve VRR

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── VTable helpers ────────────────────────────────────────────────────────────
static bool WritePtr(void** addr, void* newVal, void** oldVal) {
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

// ── Hooked Present ────────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    if (g_FirstFrame) {
        g_FirstFrame = false;
        QueryPerformanceFrequency(&g_qpcFreq);
        g_LastReleaseTime = GetTimeMs();
        
        Log("=================================================");
        Log("FIRST PRESENT INTERCEPTED - EXTRACTING TELEMETRY:");
        
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pSC->GetDesc(&desc))) {
            Logf("Resolution : %u x %u", desc.BufferDesc.Width, desc.BufferDesc.Height);
            Logf("Format     : %d", desc.BufferDesc.Format);
            Logf("RefreshRate: %u / %u", desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator);
            Logf("BufferCount: %u", desc.BufferCount);
            Logf("SwapEffect : %d (4 = FLIP_DISCARD)", desc.SwapEffect);
            Logf("Flags      : 0x%X", desc.Flags);
            Logf("Windowed   : %s", desc.Windowed ? "TRUE" : "FALSE");
        } else {
            Log("WARNING: Failed to get SwapChain Description.");
        }
        
        HWND hwnd = desc.OutputWindow;
        if (hwnd) {
            char title[256] = {};
            GetWindowTextA(hwnd, title, sizeof(title));
            Logf("Target HWND: %p | Title: '%s'", hwnd, title);
        }
        Log("=================================================");
        Logf("Minimum Gap Enforcer Active: Target = %.2f ms", MINIMUM_GAP_MS);
        Beep(1000, 150);
    }

    // --- THE 5.5ms GAP ENFORCER ---
    double now = GetTimeMs();
    double timeSinceLast = now - g_LastReleaseTime;

    if (timeSinceLast < MINIMUM_GAP_MS) {
        double targetTime = g_LastReleaseTime + MINIMUM_GAP_MS;
        // Spin lock for absolute microsecond precision
        while (GetTimeMs() < targetTime) {
            YieldProcessor(); 
        }
    }

    // Record the exact time we released the frame down the pipeline
    g_LastReleaseTime = GetTimeMs();

    return oPresent(pSC, SyncInterval, Flags);
}

// ── Init Thread (Dummy Swapchain) ─────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) { if (GetModuleHandleA("dxgi.dll")) break; Sleep(50); }
    HWND gameWnd = nullptr;
    for (int i = 0; i < 600; ++i) {
        Sleep(50);
        HWND fg = GetForegroundWindow();
        if (fg) {
            char title[256] = {}; GetWindowTextA(fg, title, sizeof(title));
            RECT r = {}; GetClientRect(fg, &r);
            if ((r.right - r.left) > 400 && title[0] != '\0') { gameWnd = fg; break; }
        }
    }
    Sleep(500);

    Logf("Game window found. Spawning dummy swapchain to hook Present.");

    WNDCLASSEXA wc = { sizeof(wc), CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyWindow", nullptr };
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowA("DummyWindow", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pDummySC = nullptr; ID3D11Device* pDummyDev = nullptr; D3D_FEATURE_LEVEL featureLevel;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySC, &pDummyDev, &featureLevel, nullptr))) {
        void** vtable = *(void***)pDummySC;
        WritePtr(&vtable[SLOT_Present], (void*)HookedPresent, (void**)&oPresent);
        pDummySC->Release(); pDummyDev->Release();
        Log("Hook installed successfully. Waiting for game to call Present.");
    } else {
        Log("ERROR: Failed to create dummy swapchain to steal vtable.");
    }

    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        
        // Setup Crash Handler & Thread Safety
        SetUnhandledExceptionFilter(CrashHandler);
        InitializeCriticalSection(&g_logCS);
        g_logCSInit = true;

        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath);
        }
        
        // Delete old log to keep it clean for this run
        remove(g_logPath);

        Log("DLL Booted - v34 Verbose Telemetry & 5.5ms Enforcer");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_logCSInit) DeleteCriticalSection(&g_logCS);
    }
    return TRUE;
}