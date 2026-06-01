// AutoPacer v38 - The Asynchronous Container (Proxy Queue)
//
// Built on the user's "Container" concept. 
// Completely decouples the Mod's submission thread from the Hardware delivery thread.
// The Mod drops frames into a thread-safe container and gets an instant S_OK.
// A dedicated background thread drips the frames to the Intel driver at a flawless
// 144 FPS (6.94ms gap). This completely shields the Intel driver from PCIe clustering.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <queue>
#include <mutex>
#include <condition_variable>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v38] %s\n", msg);
        fclose(fp);
    }
}
static void Logf(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); Log(buf);
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

// ── The Container (Thread-Safe Queue) ─────────────────────────────────────────
struct PresentArgs {
    IDXGISwapChain* pSC;
    UINT SyncInterval;
    UINT Flags;
};

static std::queue<PresentArgs> g_Queue;
static std::mutex g_Mutex;
static std::condition_variable g_CV_Produce;
static std::condition_variable g_CV_Consume;

static LARGE_INTEGER g_qpcFreq;
static double g_LastReleaseTime = 0.0;
const double TARGET_GAP_MS = 6.944; // Exactly 144 FPS to stay safely inside 165Hz VRR

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Producer) ─────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    // Put the frame into the Container
    std::unique_lock<std::mutex> lock(g_Mutex);
    
    // If our container has 2 frames in it, make the Mod wait. 
    // This acts as flawless backpressure without relying on Intel's broken queue.
    g_CV_Produce.wait(lock, [] { return g_Queue.size() < 2; });
    
    g_Queue.push({ pSC, SyncInterval, Flags });
    
    // Tell our background thread a frame is ready
    g_CV_Consume.notify_one();
    
    // Immediately tell the Mod "Success", so it can keep working unhindered
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("Asynchronous Container Thread started.");
    
    while (true) {
        PresentArgs args;
        
        // 1. Wait for a frame to enter the Container
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
        }
        
        // Tell the Mod there is free space in the Container
        g_CV_Produce.notify_one();

        // 2. The Tollbooth (Perfect 144 FPS pacing)
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + TARGET_GAP_MS;
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); // Ultra-precise micro-spin
            }
        }
        
        // 3. Deliver to Intel Driver
        g_LastReleaseTime = GetTimeMs();
        
        // We strip ALLOW_TEARING to let DWM handle the final sync natively if needed,
        // or leave it as args.Flags if VRR is preferred. We'll use args.Flags to keep VRR.
        oPresent(args.pSC, args.SyncInterval, args.Flags);
    }
    return 0;
}

// ── Init Thread ───────────────────────────────────────────────────────────────
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
        Log("Container Hooks installed successfully.");
    }
    DestroyWindow(dummyWnd); UnregisterClassA("DummyWindow", wc.hInstance);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq);
        char dllPath[MAX_PATH] = {}; GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) { *(lastSlash + 1) = '\0'; snprintf(g_logPath, sizeof(g_logPath), "%sAutoPacer.log", dllPath); }
        remove(g_logPath);
        Log("DLL Booted - v38 The Asynchronous Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}