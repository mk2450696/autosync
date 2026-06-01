// AutoPacer v40 - The True FPS Container (Non-Blocking)
//
// 1. Tracks real-time FPS natively without feedback loops.
// 2. NEVER blocks the CPU. If the proxy queue fills up, it silently drops the 
//    oldest frame instead of freezing the game engine (Fixes the 56 FPS lock & loading bug).
// 3. The background thread adapts dynamically to the tracked FPS, but strictly 
//    clamps the delivery to 155 FPS to guarantee VRR stays active on a 165Hz monitor.

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
#include <atomic>

static char g_logPath[MAX_PATH] = "AutoPacer.log";

static void Log(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, g_logPath, "a") == 0) {
        fprintf(fp, "[AutoPacer v40] %s\n", msg);
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

// ── The Container State ───────────────────────────────────────────────────────
struct PresentArgs {
    IDXGISwapChain* pSC;
    UINT SyncInterval;
    UINT Flags;
};

static std::queue<PresentArgs> g_Queue;
static std::mutex g_Mutex;
static std::condition_variable g_CV_Consume;

static LARGE_INTEGER g_qpcFreq;
static double g_LastProduceTime = 0.0;
static double g_LastReleaseTime = 0.0;

// FPS Tracker
static std::atomic<double> g_CurrentFPS{ 60.0 }; 

static double GetTimeMs() {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (double)(qpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

// ── Hooked Present (The Non-Blocking Producer) ────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSC, UINT SyncInterval, UINT Flags)
{
    double now = GetTimeMs();
    
    // 1. Track True FPS
    if (g_LastProduceTime > 0.0) {
        double gap = now - g_LastProduceTime;
        if (gap > 1.0 && gap < 100.0) { // Ignore load screen pauses
            double instantFPS = 1000.0 / gap;
            // Smooth FPS rolling average
            double current = g_CurrentFPS.load();
            g_CurrentFPS.store((current * 0.95) + (instantFPS * 0.05));
        }
    }
    g_LastProduceTime = now;

    // 2. Put Frame in the Container (NEVER BLOCK)
    {
        std::unique_lock<std::mutex> lock(g_Mutex);
        
        // If the queue has 3 frames, we are exceeding the delivery speed.
        // Silently drop the oldest frame. This guarantees the CPU is NEVER delayed.
        while (g_Queue.size() >= 3) {
            g_Queue.pop(); 
        }
        
        g_Queue.push({ pSC, SyncInterval, Flags });
    }
    
    g_CV_Consume.notify_one();
    
    // 3. Return instantly. The game runs free.
    return S_OK; 
}

// ── Background Pacer Thread (The Consumer) ────────────────────────────────────
static DWORD WINAPI PacerThread(LPVOID) {
    Log("True FPS Container Thread started. CPU blocking is DELETED.");
    int logCounter = 0;
    
    while (true) {
        PresentArgs args;
        
        // 1. Get Frame from Container
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_CV_Consume.wait(lock, [] { return !g_Queue.empty(); });
            args = g_Queue.front();
            g_Queue.pop();
        }

        // 2. Adapt to FPS, but Protect VRR
        double fps = g_CurrentFPS.load();
        if (fps > 155.0) fps = 155.0; // Hard clamp to prevent tearing on 165Hz monitor
        if (fps < 30.0) fps = 30.0;
        
        double targetGapMs = 1000.0 / fps;

        // 3. Perfect Delivery Timing
        if (g_LastReleaseTime > 0.0) {
            double targetTime = g_LastReleaseTime + targetGapMs;
            double now = GetTimeMs();
            
            // Anti-Starvation: Don't speed-up if the game paused
            if (now > targetTime + 20.0) targetTime = now;
            
            while (GetTimeMs() < targetTime) {
                YieldProcessor(); // Ultra-precise micro-spin
            }
        }
        
        // 4. Send to Intel Driver
        g_LastReleaseTime = GetTimeMs();
        oPresent(args.pSC, args.SyncInterval, args.Flags);

        // 5. Periodic Logging (Every ~5 seconds)
        logCounter++;
        if (logCounter % 600 == 0) {
            Logf("Dynamic Pacer -> Game FPS: %.1f | Delivering at: %.1f FPS | Gap: %.3f ms", 
                 g_CurrentFPS.load(), fps, targetGapMs);
        }
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
        Log("Dynamic Container Hooks installed successfully.");
        Beep(1000, 150);
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
        Log("DLL Booted - v40 The True FPS Container");
        
        CreateThread(nullptr, 0, PacerThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    return TRUE;
}